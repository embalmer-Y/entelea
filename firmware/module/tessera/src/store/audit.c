/* SPDX-License-Identifier: Apache-2.0 */
/* 审计 flash 环（DEC-52①，V2-2）：安全事件掉电留存。
 *
 * 布局：8KB 分区 = 两页 × 4KB（擦除块对齐）× 每页 64 槽 × 64B 定长。
 * 物理约束适配（对裁决的两点如实实现修正，语义等价登记 decisions）：
 *  - "单条覆盖最旧"不可行（flash 最小擦除单位 = 4KB 扇区）→ **两页乒乓**：
 *    active 页顺序写满 64 槽 → 擦对页 → 翻页续写——掉电始终保留最近
 *    ~64 条（翻页瞬间最多 128，与"覆盖最旧"的保留量等价）；
 *  - 落盘范围 = 安全异常类（commit 审计 res != TS_OK：安全态拒绝/限幅
 *    截断 + estop 沿触发）——正常输出流不落盘（audit_append 全量记录，
 *    热路径 flash 写不可行；内存环保留全量供遥测）。
 * 掉电半写探测：槽头 magic + seq 校验（半写槽 = magic 不符或 seq 乱序，
 * 扫描期丢弃）。写路径 = 事件低频（异常类），同步写可接受。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/store.h>
#include "internal.h"

#define AUDIT_MAGIC   0x54534131U /* "TSA1" */
#define AUDIT_PAGE_SZ 4096U
#define AUDIT_SLOT_SZ 64U
#define AUDIT_SLOTS   (AUDIT_PAGE_SZ / AUDIT_SLOT_SZ) /* 64 */
#define AUDIT_PAGES   2U

static struct {
	bool ready;
	uint8_t active_page;  /* 0/1 */
	uint8_t next_slot;    /* active 页内下一写槽 0..63 */
	uint32_t next_seq;    /* 全局单调（1 起；0 保留） */
} au;

/* 槽小端布局（64B）：magic|seq|t_ms|ch_idx|actor|kind|_pad|evt|res(i32)|value_u|pad */
#define OFF_MAGIC  0
#define OFF_SEQ    4
#define OFF_TMS    8
#define OFF_CHIDX 16
#define OFF_ACTOR 18
#define OFF_KIND  20
#define OFF_PAD1  21
#define OFF_EVT   22
#define OFF_RES   24  /* ts_res_t 为负值枚举——4B int32 编码（u8 会截断） */
#define OFF_VALUE 28
#define OFF_TAIL  32

/* 64 位小端（t_ms 专用——internal.h 仅有 16/32 位形态） */
static void put_le64(uint8_t *p, uint64_t v)
{
	ts_put_le32(p, (uint32_t)v);
	ts_put_le32(p + 4, (uint32_t)(v >> 32));
}

static void slot_encode(uint8_t *s, const ts_audit_entry_t *e, uint16_t evt,
			uint32_t seq)
{
	memset(s, 0xFF, AUDIT_SLOT_SZ); /* pad = 擦除态（未写满字节） */
	ts_put_le32(s + OFF_MAGIC, AUDIT_MAGIC);
	ts_put_le32(s + OFF_SEQ, seq);
	put_le64(s + OFF_TMS, e->t_ms);
	ts_put_le16(s + OFF_CHIDX, e->ch_idx);
	ts_put_le16(s + OFF_ACTOR, e->actor);
	s[OFF_KIND] = e->kind;
	s[OFF_PAD1] = 0xFF;
	ts_put_le16(s + OFF_EVT, evt);
	ts_put_le32(s + OFF_RES, (uint32_t)(int32_t)e->res);
	ts_put_le32(s + OFF_VALUE, e->value_u);
}

static void slot_decode(const uint8_t *s, ts_store_audit_entry_t *e)
{
	e->seq = ts_get_le32(s + OFF_SEQ);
	e->t_ms = (uint64_t)ts_get_le32(s + OFF_TMS) |
		  ((uint64_t)ts_get_le32(s + OFF_TMS + 4) << 32);
	e->ch_idx = ts_get_le16(s + OFF_CHIDX);
	e->actor = ts_get_le16(s + OFF_ACTOR);
	e->kind = s[OFF_KIND];
	e->res = (ts_res_t)(int32_t)ts_get_le32(s + OFF_RES);
	e->evt = ts_get_le16(s + OFF_EVT);
	e->value_u = ts_get_le32(s + OFF_VALUE);
}

/* 页扫描：返回该页最大 seq 槽（无有效槽 = false）。掉电半写容错：
 * 顺序写假设下，自首个坏槽起其后全部忽略（断尾）；乱序 seq 亦截断。 */
static bool page_scan(uint8_t page, uint32_t *max_seq, uint8_t *used_slots)
{
	uint8_t slot[AUDIT_SLOT_SZ];
	uint32_t prev = 0;

	*max_seq = 0;
	*used_slots = 0;
	for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
		uint32_t off = page * AUDIT_PAGE_SZ + i * AUDIT_SLOT_SZ;

		if (ts_store_backend.read(TS_PART_AUDIT, off, slot,
					  AUDIT_SLOT_SZ) != TS_OK) {
			return false;
		}
		if (ts_get_le32(slot + OFF_MAGIC) != AUDIT_MAGIC) {
			break; /* 未写/半写 → 断尾 */
		}
		uint32_t seq = ts_get_le32(slot + OFF_SEQ);

		if (seq == 0 || (i > 0 && seq != prev + 1)) {
			break; /* 序断裂（半写）→ 断尾 */
		}
		prev = seq;
		*max_seq = seq;
		(*used_slots)++;
	}
	return *used_slots > 0;
}

ts_res_t ts_store_audit_init(void)
{
	uint32_t seq_a, seq_b;
	uint8_t used_a, used_b;
	bool has_a = page_scan(0, &seq_a, &used_a);
	bool has_b = page_scan(1, &seq_b, &used_b);

	if (!has_a && !has_b) {
		/* 全新分区：页 0 起（页 0 为擦除态——首 append 前无需擦） */
		au.active_page = 0;
		au.next_slot = 0;
		au.next_seq = 1;
	} else if (seq_b > seq_a || (has_b && !has_a)) {
		au.active_page = 1;
		au.next_seq = seq_b + 1;
		au.next_slot = (used_b == AUDIT_SLOTS) ? 0 : used_b;
		if (used_b == AUDIT_SLOTS) {
			/* 满页未翻：翻页须擦页 0（可能含更旧数据）——append 期做 */
			au.active_page = 0;
		}
	} else {
		au.active_page = 0;
		au.next_seq = seq_a + 1;
		au.next_slot = (used_a == AUDIT_SLOTS) ? 0 : used_a;
		if (used_a == AUDIT_SLOTS) {
			au.active_page = 1;
		}
	}
	/* 翻页目标页含旧数据时 append 首条前擦（见 append） */
	au.ready = true;
	return TS_OK;
}

ts_res_t ts_store_audit_append(const ts_audit_entry_t *e, uint16_t evt)
{
	if (!au.ready) {
		return TS_E_STATE;
	}
	uint8_t slot[AUDIT_SLOT_SZ];

	slot_encode(slot, e, evt, au.next_seq);
	uint32_t off = au.active_page * AUDIT_PAGE_SZ + au.next_slot * AUDIT_SLOT_SZ;

	if (au.next_slot == 0) {
		/* 页首写：确保目标页为擦除态（全新分区首页/翻页后的对页）。
		 * 擦除幂等（已擦区再擦合法）——翻页瞬间掉电的恢复路径。 */
		if (ts_store_backend.erase_off(TS_PART_AUDIT,
					       au.active_page * AUDIT_PAGE_SZ,
					       AUDIT_PAGE_SZ) != TS_OK) {
			return TS_E_IO;
		}
	}
	if (ts_store_backend.write(TS_PART_AUDIT, off, slot,
				   AUDIT_SLOT_SZ) != TS_OK) {
		return TS_E_IO;
	}
	au.next_seq++;
	au.next_slot++;
	if (au.next_slot == AUDIT_SLOTS) {
		au.next_slot = 0;
		au.active_page ^= 1U; /* 写满翻页（对页在下次页首写前擦） */
	}
	return TS_OK;
}

size_t ts_store_audit_read(ts_store_audit_entry_t *out, size_t max)
{
	if (!au.ready) {
		return 0;
	}
	/* 两页收集（≤128 过渡态）后按 seq 取最近 max 条。
	 * all 为静态（128×~32B ≈ 4KB——真机线程栈 4096 实测爆栈拦下）；
	 * 读路径低频且单消费（V1 无并发读面）。 */
	static ts_store_audit_entry_t all[AUDIT_SLOTS * AUDIT_PAGES];
	size_t n = 0;

	for (uint8_t p = 0; p < AUDIT_PAGES; p++) {
		for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
			uint8_t slot[AUDIT_SLOT_SZ];
			uint32_t off = p * AUDIT_PAGE_SZ + i * AUDIT_SLOT_SZ;

			if (ts_store_backend.read(TS_PART_AUDIT, off, slot,
						  AUDIT_SLOT_SZ) != TS_OK) {
				continue;
			}
			if (ts_get_le32(slot + OFF_MAGIC) != AUDIT_MAGIC) {
				break;
			}
			slot_decode(slot, &all[n]);
			n++;
		}
	}
	/* 插入排序（seq 升序；n ≤ 128 读路径低频） */
	for (size_t i = 1; i < n; i++) {
		ts_store_audit_entry_t k = all[i];
		size_t j = i;

		while (j > 0 && all[j - 1].seq > k.seq) {
			all[j] = all[j - 1];
			j--;
		}
		all[j] = k;
	}
	/* 断裂容错：跨页 seq 非连续（半写截断形态）时丢弃旧页断链段——
	 * 以最大连续尾段为可信集（朴素实现：直接返回排序尾部 max 条，
	 * 断链槽的 seq 本身小于尾部——半写槽已在 page_scan 断尾语义内） */
	size_t take = (n > max) ? max : n;

	memcpy(out, &all[n - take], take * sizeof(out[0]));
	return take;
}

void ts_store_audit_boot_report(void)
{
	ts_store_audit_entry_t tail[8];
	size_t n = ts_store_audit_read(tail, 8);

	if (n == 0) {
		printk("[audit] flash 环空（无历史安全事件）\n");
		return;
	}
	printk("[audit] 最近 %u 条安全事件（flash 环留存，DEC-52）：\n", (unsigned)n);
	for (size_t i = 0; i < n; i++) {
		printk("[audit]  seq=%u t=%llums ch=%u res=%d evt=%u val=%u actor=%u\n",
		       (unsigned)tail[i].seq, (long long)tail[i].t_ms,
		       (unsigned)tail[i].ch_idx, (int)tail[i].res,
		       (unsigned)tail[i].evt, (unsigned)tail[i].value_u,
		       (unsigned)tail[i].actor);
	}
}
