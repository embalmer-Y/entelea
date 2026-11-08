/* SPDX-License-Identifier: Apache-2.0 */
/*
 * APP 业务状态 kv（迁自旧 store/kv.c，R03 合同见本目录 contract.md）：
 * 每 APP 一份有界 blob（≤250B），跨升级/回滚按 app_id 哈希归属保留。
 * 布局：8KB = 两页 × 4KB × 15 槽 × 264B（14B 头 + ≤250B blob）。
 * 追加式写（同 app 重存 = 新槽，load 取最大 seq 匹配槽）；页满 →
 * 压实翻页（每 app 最新重写至对页，先写后切——压实窗口掉电原页保全，
 * 最坏丢压实开始后未及重写的 app 状态，如实接受）。半写断尾同 audit。
 * 容量：至多 15 个 app（压实保 KV_SLOTS-1 为新写留余量；超出弃 seq
 * 最旧 app——有界语义如实登记）。250B blob 不是经历库（合同既有约束）。
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define KV_MAGIC   0x54534B31U /* "TSK1" */
#define KV_PAGE_SZ 4096U
#define KV_SLOT_SZ 264U
#define KV_SLOTS   (KV_PAGE_SZ / KV_SLOT_SZ) /* 15 */
#define KV_PAGES   2U

#define KV_OFF_MAGIC 0
#define KV_OFF_HASH  4
#define KV_OFF_SEQ   8
#define KV_OFF_LEN   12
#define KV_OFF_DATA  14

static struct {
	bool ready;
	uint8_t active_page;
	uint8_t next_slot;
	uint32_t next_seq; /* 全局单调（1 起） */
} kv;

static bool page_scan(uint8_t page, uint32_t *max_seq, uint8_t *used)
{
	uint8_t slot[KV_SLOT_SZ];
	uint32_t prev = 0;

	*max_seq = 0;
	*used = 0;
	for (uint8_t i = 0; i < KV_SLOTS; i++) {
		uint32_t off = page * KV_PAGE_SZ + i * KV_SLOT_SZ;

		if (entelea_store_backend.read(ENTELEA_PART_KV, off, slot,
					       KV_SLOT_SZ) != ENTELEA_OK) {
			return false;
		}
		if (entelea_get_le32(slot + KV_OFF_MAGIC) != KV_MAGIC) {
			break; /* 未写/半写 → 断尾 */
		}
		uint32_t seq = entelea_get_le32(slot + KV_OFF_SEQ);

		if (seq == 0 || (i > 0 && seq != prev + 1)) {
			break;
		}
		prev = seq;
		*max_seq = seq;
		(*used)++;
	}
	return *used > 0;
}

static void slot_encode(uint8_t *s, uint32_t hash, const uint8_t *data,
			uint16_t len, uint32_t seq)
{
	memset(s, 0xFF, KV_SLOT_SZ);
	entelea_put_le32(s + KV_OFF_MAGIC, KV_MAGIC);
	entelea_put_le32(s + KV_OFF_HASH, hash);
	entelea_put_le32(s + KV_OFF_SEQ, seq);
	entelea_put_le16(s + KV_OFF_LEN, len);
	memcpy(s + KV_OFF_DATA, data, len);
}

#define KV_KEEP_MAX (KV_SLOTS - 1)
struct kv_ref {
	uint32_t hash;
	uint32_t seq;
	uint16_t len;
	uint8_t page;
	uint8_t slot;
};

static size_t collect_latest(struct kv_ref *refs)
{
	size_t n = 0;
	uint8_t slot[KV_SLOT_SZ];

	for (uint8_t p = 0; p < KV_PAGES; p++) {
		for (uint8_t i = 0; i < KV_SLOTS; i++) {
			uint32_t off = p * KV_PAGE_SZ + i * KV_SLOT_SZ;

			if (entelea_store_backend.read(ENTELEA_PART_KV, off, slot,
						       KV_SLOT_SZ) != ENTELEA_OK ||
			    entelea_get_le32(slot + KV_OFF_MAGIC) != KV_MAGIC) {
				break;
			}
			uint32_t hash = entelea_get_le32(slot + KV_OFF_HASH);
			uint32_t seq = entelea_get_le32(slot + KV_OFF_SEQ);
			size_t hit = n;
			bool found = false;

			for (size_t k = 0; k < n; k++) {
				if (refs[k].hash == hash) {
					hit = k;
					found = true;
					break;
				}
			}
			if (found) {
				if (refs[hit].seq > seq) {
					continue;
				}
			} else if (n == KV_KEEP_MAX) {
				size_t oldest = 0;

				for (size_t k = 1; k < n; k++) {
					if (refs[k].seq < refs[oldest].seq) {
						oldest = k;
					}
				}
				if (refs[oldest].seq > seq) {
					continue;
				}
				hit = oldest;
			} else {
				hit = n++;
			}
			refs[hit].hash = hash;
			refs[hit].seq = seq;
			refs[hit].len = entelea_get_le16(slot + KV_OFF_LEN);
			refs[hit].page = p;
			refs[hit].slot = i;
		}
	}
	return n;
}

/* 公共 API（信任表头文件独立；kv 面并入 storage.h）。 */
#include <entelea/kv.h>

int32_t entelea_kv_load(uint32_t app_hash, uint8_t *out, uint16_t max)
{
	if (entelea_kv_init() != ENTELEA_OK) {
		return ENTELEA_E_STATE;
	}
	uint8_t slot[KV_SLOT_SZ];
	int32_t best = -1;
	uint32_t best_seq = 0;
	uint16_t best_len = 0;

	for (uint8_t p = 0; p < KV_PAGES; p++) {
		for (uint8_t i = 0; i < KV_SLOTS; i++) {
			uint32_t off = p * KV_PAGE_SZ + i * KV_SLOT_SZ;

			if (entelea_store_backend.read(ENTELEA_PART_KV, off, slot,
						       KV_SLOT_SZ) != ENTELEA_OK ||
			    entelea_get_le32(slot + KV_OFF_MAGIC) != KV_MAGIC) {
				break;
			}
			if (entelea_get_le32(slot + KV_OFF_HASH) != app_hash) {
				continue;
			}
			uint32_t seq = entelea_get_le32(slot + KV_OFF_SEQ);

			if (best < 0 || seq > best_seq) {
				best = (int32_t)i;
				best_seq = seq;
				best_len = entelea_get_le16(slot + KV_OFF_LEN);
				if (best_len > max) {
					return ENTELEA_E_RANGE; /* 容量不足如实 */
				}
				memcpy(out, slot + KV_OFF_DATA, best_len);
			}
		}
	}
	return (best < 0) ? ENTELEA_E_NOTFOUND : (int32_t)best_len;
}

entelea_result_t entelea_kv_save(uint32_t app_hash, const uint8_t *data,
				 uint16_t len)
{
	if (data == NULL || len == 0 || len > ENTELEA_KV_BLOB_MAX) {
		return ENTELEA_E_PARAM;
	}
	if (entelea_kv_init() != ENTELEA_OK) {
		return ENTELEA_E_STATE;
	}
	if (kv.next_slot == KV_SLOTS) {
		/* 页满：压实翻页（每 app 最新重写至对页，先写后切）。 */
		static struct kv_ref refs[KV_KEEP_MAX];
		size_t n = collect_latest(refs);
		uint8_t target = kv.active_page ^ 1U;

		if (entelea_store_backend.erase_off(ENTELEA_PART_KV,
						    target * KV_PAGE_SZ,
						    KV_PAGE_SZ) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		for (size_t i = 0; i < n; i++) {
			uint8_t slot[KV_SLOT_SZ];
			uint32_t src =
				refs[i].page * KV_PAGE_SZ + refs[i].slot * KV_SLOT_SZ;

			if (entelea_store_backend.read(ENTELEA_PART_KV, src, slot,
						       KV_SLOT_SZ) != ENTELEA_OK) {
				return ENTELEA_E_IO;
			}
			entelea_put_le32(slot + KV_OFF_SEQ, kv.next_seq);
			if (entelea_store_backend.write(
				    ENTELEA_PART_KV,
				    target * KV_PAGE_SZ + i * KV_SLOT_SZ, slot,
				    KV_SLOT_SZ) != ENTELEA_OK) {
				return ENTELEA_E_IO;
			}
			kv.next_seq++;
		}
		kv.active_page = target;
		kv.next_slot = (uint8_t)n;
	}
	uint8_t slot[KV_SLOT_SZ];

	slot_encode(slot, app_hash, data, len, kv.next_seq);
	if (entelea_store_backend.write(ENTELEA_PART_KV,
					kv.active_page * KV_PAGE_SZ +
						kv.next_slot * KV_SLOT_SZ,
					slot, KV_SLOT_SZ) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	kv.next_seq++;
	kv.next_slot++;
	return ENTELEA_OK;
}

entelea_result_t entelea_kv_init(void)
{
	if (kv.ready) {
		return ENTELEA_OK;
	}
	if (entelea_store_backend.size(ENTELEA_PART_KV) < KV_PAGES * KV_PAGE_SZ) {
		return ENTELEA_E_STATE; /* 分区未编入——如实降级 */
	}
	uint32_t sa, sb;
	uint8_t ua, ub;
	bool ha = page_scan(0, &sa, &ua);
	bool hb = page_scan(1, &sb, &ub);

	if (!ha && !hb) {
		kv.active_page = 0;
		kv.next_slot = 0;
		kv.next_seq = 1;
	} else if (sb > sa || (hb && !ha)) {
		kv.active_page = 1;
		kv.next_seq = sb + 1;
		kv.next_slot = (ub == KV_SLOTS) ? 0 : ub;
		if (ub == KV_SLOTS) {
			kv.active_page = 0; /* 满页未压实：下次 save 前 compact */
		}
	} else {
		kv.active_page = 0;
		kv.next_seq = sa + 1;
		kv.next_slot = (ua == KV_SLOTS) ? 0 : ua;
		if (ua == KV_SLOTS) {
			kv.active_page = 1;
		}
	}
	kv.ready = true;
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_kv_test_reset(void)
{
	memset(&kv, 0, sizeof(kv));
}
#endif
