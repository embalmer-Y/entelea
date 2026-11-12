/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 审计两页乒乓环（迁自旧 store/audit.c，R03 合同见本目录 contract.md）：
 * 安全异常类掉电留存。8KB = 两页 × 4KB（擦除块对齐）× 64 槽 × 64B 定长。
 * "单条覆盖最旧"不可行（擦除单位=页）→ 两页乒乓：active 页写满 → 翻页
 * 续写（页首写前擦对页）——掉电始终保留最近 ~64 条（翻页瞬间 ≤128）。
 * RF1（review P1-6）：体及 CRC、原 magic、独立提交标记依次写；
 * 未提交槽跳过，已提交的损坏槽显式报错；读错误使 init 不就绪、
 * read 返回负错误码；写失败烧毁该槽与 seq。
 * 相对旧实现调整：boot 打印留在平台层（本模块 printk-free，经 read API
 * 消费）。全量内存环保留（旧 commit.c 面）归 R04 观测面，本环只落异常类。
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define AUDIT_MAGIC 0x54534131U /* "TSA1" */
#define AUDIT_PAGE_SZ 4096U
#define AUDIT_SLOT_SZ 64U
#define AUDIT_SLOTS (AUDIT_PAGE_SZ / AUDIT_SLOT_SZ) /* 64 */
#define AUDIT_PAGES 2U

static struct {
	bool ready;
	uint8_t active_page; /* 0/1 */
	uint8_t next_slot; /* active 页内下一写槽 0..63 */
	uint32_t next_seq; /* 全局单调（1 起；0/0xFFFFFFFF 不用） */
} au;

/* 槽小端布局（64B）：magic|seq|t_ms|ch_idx|actor|kind|_pad|evt|res|
 * value|crc16|tag|commit|pad。旧槽的 crc/tag/commit 均为擦除值。 */
#define OFF_MAGIC 0
#define OFF_SEQ 4
#define OFF_TMS 8
#define OFF_CHIDX 16
#define OFF_ACTOR 18
#define OFF_KIND 20
#define OFF_PAD1 21
#define OFF_EVT 22
#define OFF_RES 24
#define OFF_VALUE 28
#define OFF_CRC 32
#define OFF_TAG 34
#define OFF_COMMIT 36
#define AUDIT_TAG_MAGIC 0xA56CU
#define AUDIT_TAG_COMMIT 0x31435641U

enum slot_kind {
	SLOT_INCOMPLETE,
	SLOT_VALID,
	SLOT_CORRUPT,
};

static void
put_le64(uint8_t *p, uint64_t v)
{
	entelea_put_le32(p, (uint32_t) v);
	entelea_put_le32(p + 4, (uint32_t) (v >> 32));
}

static void
slot_encode(uint8_t *s, const entelea_storage_audit_entry_t *e, uint16_t evt,
	    uint32_t seq)
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
	entelea_put_le32(s + OFF_RES, (uint32_t) (int32_t) e->res);
	entelea_put_le32(s + OFF_VALUE, e->value_u);
	entelea_put_le16(s + OFF_CRC,
			 entelea_crc16(s + OFF_SEQ, OFF_CRC - OFF_SEQ));
	entelea_put_le16(s + OFF_TAG, AUDIT_TAG_MAGIC);
	entelea_put_le32(s + OFF_COMMIT, AUDIT_TAG_COMMIT);
}

static void
slot_decode(const uint8_t *s, entelea_storage_audit_entry_t *e)
{
	e->seq = entelea_get_le32(s + OFF_SEQ);
	e->t_ms = (uint64_t) entelea_get_le32(s + OFF_TMS) |
		  ((uint64_t) entelea_get_le32(s + OFF_TMS + 4) << 32);
	e->ch_idx = entelea_get_le16(s + OFF_CHIDX);
	e->actor = entelea_get_le16(s + OFF_ACTOR);
	e->kind = s[OFF_KIND];
	e->res = (entelea_result_t) (int32_t) entelea_get_le32(s + OFF_RES);
	e->evt = entelea_get_le16(s + OFF_EVT);
	e->value_u = entelea_get_le32(s + OFF_VALUE);
}

static entelea_result_t
slot_read(uint8_t page, uint8_t i, uint8_t *s)
{
	return entelea_store_backend.read(ENTELEA_PART_AUDIT,
					  page * AUDIT_PAGE_SZ +
						  i * AUDIT_SLOT_SZ,
					  s, AUDIT_SLOT_SZ) == ENTELEA_OK ?
		       ENTELEA_OK :
		       ENTELEA_E_IO;
}

static enum slot_kind
slot_kind(const uint8_t *s)
{
	uint32_t seq = entelea_get_le32(s + OFF_SEQ);
	uint32_t commit = entelea_get_le32(s + OFF_COMMIT);
	bool fields_valid = entelea_get_le32(s + OFF_MAGIC) == AUDIT_MAGIC &&
			    seq != 0 && seq != UINT32_MAX;
	bool legacy = entelea_get_le32(s + OFF_CRC) == UINT32_MAX &&
		      commit == UINT32_MAX;

	if (legacy) {
		return fields_valid ? SLOT_VALID : SLOT_INCOMPLETE;
	}
	if (commit == UINT32_MAX) {
		return SLOT_INCOMPLETE;
	}
	if (commit != AUDIT_TAG_COMMIT ||
	    entelea_get_le16(s + OFF_TAG) != AUDIT_TAG_MAGIC || !fields_valid ||
	    entelea_get_le16(s + OFF_CRC) !=
		    entelea_crc16(s + OFF_SEQ, OFF_CRC - OFF_SEQ)) {
		return SLOT_CORRUPT;
	}
	return SLOT_VALID;
}

static bool
slot_erased(const uint8_t *s)
{
	for (uint32_t k = 0; k < AUDIT_SLOT_SZ; k++) {
		if (s[k] != 0xFF) {
			return false;
		}
	}
	return true;
}

static entelea_result_t
slot_commit(uint8_t page, uint8_t i, const uint8_t *s)
{
	uint32_t off = page * AUDIT_PAGE_SZ + i * AUDIT_SLOT_SZ;
	uint8_t back[AUDIT_SLOT_SZ];

	if (entelea_store_backend.write(ENTELEA_PART_AUDIT, off + OFF_SEQ,
					s + OFF_SEQ,
					OFF_COMMIT - OFF_SEQ) != ENTELEA_OK ||
	    entelea_store_backend.write(ENTELEA_PART_AUDIT, off, s, OFF_SEQ) !=
		    ENTELEA_OK ||
	    entelea_store_backend.write(ENTELEA_PART_AUDIT, off + OFF_COMMIT,
					s + OFF_COMMIT, 4) != ENTELEA_OK ||
	    slot_read(page, i, back) != ENTELEA_OK ||
	    memcmp(back, s, sizeof(back)) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

/* 页扫描：最大有效 seq + 已用槽数（末个非擦除槽 + 1；坏槽不复用、
 * 不断尾——其后有效槽照常可见）。读错误显式失败。 */
static entelea_result_t
page_scan(uint8_t page, uint32_t *max_seq, uint8_t *used_slots)
{
	uint8_t s[AUDIT_SLOT_SZ];

	*max_seq = 0;
	*used_slots = 0;
	for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
		if (slot_read(page, i, s) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		enum slot_kind kind = slot_kind(s);

		if (kind == SLOT_CORRUPT) {
			return ENTELEA_E_IO;
		}
		if (kind == SLOT_VALID &&
		    entelea_get_le32(s + OFF_SEQ) > *max_seq) {
			*max_seq = entelea_get_le32(s + OFF_SEQ);
		}
		if (!slot_erased(s)) {
			*used_slots = (uint8_t) (i + 1);
		}
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_storage_audit_init(void)
{
	uint32_t seq_a;
	uint32_t seq_b;
	uint8_t used_a;
	uint8_t used_b;

	au.ready = false;
	if (page_scan(0, &seq_a, &used_a) != ENTELEA_OK ||
	    page_scan(1, &seq_b, &used_b) != ENTELEA_OK) {
		return ENTELEA_E_IO; /* 读错误≠空环：不就绪、不擦页 */
	}
	uint8_t page = (seq_b > seq_a) ? 1U : 0U;
	uint8_t used = page ? used_b : used_a;

	au.next_seq = ((seq_a > seq_b) ? seq_a : seq_b) + 1U;
	if (used >= AUDIT_SLOTS) {
		page ^= 1U; /* 满页：翻页目标在首 append 前擦 */
		used = 0;
	}
	au.active_page = page;
	au.next_slot = used;
	au.ready = true;
	return ENTELEA_OK;
}

entelea_result_t
entelea_storage_audit_append(const entelea_storage_audit_entry_t *e,
			     uint16_t evt)
{
	if (e == NULL || !au.ready || au.next_seq == UINT32_MAX) {
		return ENTELEA_E_STATE;
	}
	if (au.next_slot == 0) {
		/* 页首写：确保目标页为擦除态（翻页瞬间掉电的恢复路径；
		 * 擦除幂等；失败不消耗槽/seq）。 */
		if (entelea_store_backend.erase_off(
			    ENTELEA_PART_AUDIT, au.active_page * AUDIT_PAGE_SZ,
			    AUDIT_PAGE_SZ) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
	}
	uint8_t slot[AUDIT_SLOT_SZ];

	slot_encode(slot, e, evt, au.next_seq);
	entelea_result_t r = slot_commit(au.active_page, au.next_slot, slot);

	au.next_seq++; /* 失败亦烧毁槽与 seq：已编程字节不重写 */
	au.next_slot++;
	if (au.next_slot == AUDIT_SLOTS) {
		au.next_slot = 0;
		au.active_page ^= 1U; /* 写满翻页（对页在下次页首写前擦） */
	}
	if (r != ENTELEA_OK) {
		au.ready = false;
	}
	return r;
}

int32_t
entelea_storage_audit_read(entelea_storage_audit_entry_t *out, size_t max)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (!au.ready) {
		return ENTELEA_E_STATE;
	}
	/* 两页收集（≤128 过渡态）后按 seq 取最近 max 条。静态收集缓冲
	 * （旧真机实测：4KB 线程栈上爆栈）；读路径低频单消费。 */
	static entelea_storage_audit_entry_t all[AUDIT_SLOTS * AUDIT_PAGES];
	size_t n = 0;

	for (uint8_t p = 0; p < AUDIT_PAGES; p++) {
		for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
			uint8_t slot[AUDIT_SLOT_SZ];

			if (slot_read(p, i, slot) != ENTELEA_OK) {
				au.ready = false;
				return ENTELEA_E_IO;
			}
			enum slot_kind kind = slot_kind(slot);

			if (kind == SLOT_CORRUPT) {
				au.ready = false;
				return ENTELEA_E_IO;
			}
			if (kind == SLOT_VALID) {
				slot_decode(slot, &all[n]);
				n++;
			}
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
	return (int32_t) take;
}
