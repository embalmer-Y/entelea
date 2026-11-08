/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 审计两页乒乓环（迁自旧 store/audit.c，R03 合同见本目录 contract.md）：
 * 安全异常类掉电留存。8KB = 两页 × 4KB（擦除块对齐）× 64 槽 × 64B 定长。
 * "单条覆盖最旧"不可行（擦除单位=页）→ 两页乒乓：active 页写满 → 翻页
 * 续写（页首写前擦对页）——掉电始终保留最近 ~64 条（翻页瞬间 ≤128）。
 * 掉电半写探测：槽头 magic + seq 连续性（断尾丢弃）。
 * 相对旧实现调整：boot 打印留在平台层（本模块 printk-free，经 read API
 * 消费）。全量内存环保留（旧 commit.c 面）归 R04 观测面，本环只落异常类。
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define AUDIT_MAGIC   0x54534131U /* "TSA1" */
#define AUDIT_PAGE_SZ 4096U
#define AUDIT_SLOT_SZ 64U
#define AUDIT_SLOTS   (AUDIT_PAGE_SZ / AUDIT_SLOT_SZ) /* 64 */
#define AUDIT_PAGES   2U

static struct {
	bool ready;
	uint8_t active_page; /* 0/1 */
	uint8_t next_slot;   /* active 页内下一写槽 0..63 */
	uint32_t next_seq;   /* 全局单调（1 起；0 保留） */
} au;

/* 槽小端布局（64B）：magic|seq|t_ms|ch_idx|actor|kind|_pad|evt|res|value|pad */
#define OFF_MAGIC  0
#define OFF_SEQ    4
#define OFF_TMS    8
#define OFF_CHIDX 16
#define OFF_ACTOR 18
#define OFF_KIND  20
#define OFF_PAD1  21
#define OFF_EVT   22
#define OFF_RES   24
#define OFF_VALUE 28

static void put_le64(uint8_t *p, uint64_t v)
{
	entelea_put_le32(p, (uint32_t)v);
	entelea_put_le32(p + 4, (uint32_t)(v >> 32));
}

static void slot_encode(uint8_t *s, const entelea_storage_audit_entry_t *e,
			uint16_t evt, uint32_t seq)
{
	memset(s, 0xFF, AUDIT_SLOT_SZ); /* pad = 擦除态 */
	entelea_put_le32(s + OFF_MAGIC, AUDIT_MAGIC);
	entelea_put_le32(s + OFF_SEQ, seq);
	put_le64(s + OFF_TMS, e->t_ms);
	entelea_put_le16(s + OFF_CHIDX, e->ch_idx);
	entelea_put_le16(s + OFF_ACTOR, e->actor);
	s[OFF_KIND] = e->kind;
	s[OFF_PAD1] = 0xFF;
	entelea_put_le16(s + OFF_EVT, evt);
	entelea_put_le32(s + OFF_RES, (uint32_t)(int32_t)e->res);
	entelea_put_le32(s + OFF_VALUE, e->value_u);
}

static void slot_decode(const uint8_t *s, entelea_storage_audit_entry_t *e)
{
	e->seq = entelea_get_le32(s + OFF_SEQ);
	e->t_ms = (uint64_t)entelea_get_le32(s + OFF_TMS) |
		  ((uint64_t)entelea_get_le32(s + OFF_TMS + 4) << 32);
	e->ch_idx = entelea_get_le16(s + OFF_CHIDX);
	e->actor = entelea_get_le16(s + OFF_ACTOR);
	e->kind = s[OFF_KIND];
	e->res = (entelea_result_t)(int32_t)entelea_get_le32(s + OFF_RES);
	e->evt = entelea_get_le16(s + OFF_EVT);
	e->value_u = entelea_get_le32(s + OFF_VALUE);
}

/* 页扫描：返回该页最大 seq（无有效槽 = false）。半写容错：顺序写假设下
 * 自首个坏槽起其后全部忽略（断尾）；seq 断裂亦截断。 */
static bool page_scan(uint8_t page, uint32_t *max_seq, uint8_t *used_slots)
{
	uint8_t slot[AUDIT_SLOT_SZ];
	uint32_t prev = 0;

	*max_seq = 0;
	*used_slots = 0;
	for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
		uint32_t off = page * AUDIT_PAGE_SZ + i * AUDIT_SLOT_SZ;

		if (entelea_store_backend.read(ENTELEA_PART_AUDIT, off, slot,
					       AUDIT_SLOT_SZ) != ENTELEA_OK) {
			return false;
		}
		if (entelea_get_le32(slot + OFF_MAGIC) != AUDIT_MAGIC) {
			break; /* 未写/半写 → 断尾 */
		}
		uint32_t seq = entelea_get_le32(slot + OFF_SEQ);

		if (seq == 0 || (i > 0 && seq != prev + 1)) {
			break; /* 序断裂（半写）→ 断尾 */
		}
		prev = seq;
		*max_seq = seq;
		(*used_slots)++;
	}
	return *used_slots > 0;
}

entelea_result_t entelea_storage_audit_init(void)
{
	uint32_t seq_a, seq_b;
	uint8_t used_a, used_b;
	bool has_a = page_scan(0, &seq_a, &used_a);
	bool has_b = page_scan(1, &seq_b, &used_b);

	if (!has_a && !has_b) {
		/* 全新分区：页 0 起（擦除态，首 append 前无需擦）。 */
		au.active_page = 0;
		au.next_slot = 0;
		au.next_seq = 1;
	} else if (seq_b > seq_a || (has_b && !has_a)) {
		au.active_page = 1;
		au.next_seq = seq_b + 1;
		au.next_slot = (used_b == AUDIT_SLOTS) ? 0 : used_b;
		if (used_b == AUDIT_SLOTS) {
			/* 满页未翻：翻页目标（页 0 含旧数据）append 期擦。 */
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
	au.ready = true;
	return ENTELEA_OK;
}

entelea_result_t entelea_storage_audit_append(
	const entelea_storage_audit_entry_t *e, uint16_t evt)
{
	if (e == NULL || !au.ready) {
		return ENTELEA_E_STATE;
	}
	uint8_t slot[AUDIT_SLOT_SZ];

	slot_encode(slot, e, evt, au.next_seq);
	uint32_t off = au.active_page * AUDIT_PAGE_SZ + au.next_slot * AUDIT_SLOT_SZ;

	if (au.next_slot == 0) {
		/* 页首写：确保目标页为擦除态（翻页瞬间掉电的恢复路径；
		 * 擦除幂等）。 */
		if (entelea_store_backend.erase_off(ENTELEA_PART_AUDIT,
						    au.active_page * AUDIT_PAGE_SZ,
						    AUDIT_PAGE_SZ) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
	}
	if (entelea_store_backend.write(ENTELEA_PART_AUDIT, off, slot,
					AUDIT_SLOT_SZ) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	au.next_seq++;
	au.next_slot++;
	if (au.next_slot == AUDIT_SLOTS) {
		au.next_slot = 0;
		au.active_page ^= 1U; /* 写满翻页（对页在下次页首写前擦） */
	}
	return ENTELEA_OK;
}

size_t entelea_storage_audit_read(entelea_storage_audit_entry_t *out, size_t max)
{
	if (!au.ready || out == NULL) {
		return 0;
	}
	/* 两页收集（≤128 过渡态）后按 seq 取最近 max 条。静态收集缓冲
	 * （旧真机实测：4KB 线程栈上爆栈）；读路径低频单消费。 */
	static entelea_storage_audit_entry_t all[AUDIT_SLOTS * AUDIT_PAGES];
	size_t n = 0;

	for (uint8_t p = 0; p < AUDIT_PAGES; p++) {
		for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
			uint8_t slot[AUDIT_SLOT_SZ];
			uint32_t off = p * AUDIT_PAGE_SZ + i * AUDIT_SLOT_SZ;

			if (entelea_store_backend.read(ENTELEA_PART_AUDIT, off, slot,
						       AUDIT_SLOT_SZ) != ENTELEA_OK) {
				continue;
			}
			if (entelea_get_le32(slot + OFF_MAGIC) != AUDIT_MAGIC) {
				break;
			}
			slot_decode(slot, &all[n]);
			n++;
		}
	}
	/* 插入排序（seq 升序；n ≤ 128 低频）。 */
	for (size_t i = 1; i < n; i++) {
		entelea_storage_audit_entry_t k = all[i];
		size_t j = i;

		while (j > 0 && all[j - 1].seq > k.seq) {
			all[j] = all[j - 1];
			j--;
		}
		all[j] = k;
	}
	size_t take = (n > max) ? max : n;

	memcpy(out, &all[n - take], take * sizeof(out[0]));
	return take;
}
