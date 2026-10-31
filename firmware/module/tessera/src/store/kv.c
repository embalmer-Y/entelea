/* SPDX-License-Identifier: Apache-2.0 */
/* APP 业务状态 kv（V0.4.0 M9，DR-15）：每 APP 一份有界 blob，跨升级/回滚
 * 按 app_id 哈希归属保留（DR-15 的「升级后状态丢弃」就此收口）。
 *
 * 布局：8KB 分区 = 两页 × 4KB（擦除块对齐）× 每页 15 槽 × 264B 定长
 *（14B 头 + ≤250B blob，页尾 136B 余量）。写路径 = 追加式（同 app 重存 =
 * 追加新槽，load 取最大 seq 匹配槽——免就地改写）；active 页写满 →
 * **压实翻页**：每 app 取最新一槽（按 seq 保留至多 15 个 app——超出弃最旧
 * app 的状态，有界语义如实登记）→ 擦对页 → 顺序重写 → 切 active。
 * 掉电半写探测：槽头 magic + 页内 seq 连续性（audit.c 同型，教训 52①）。
 * 压实窗口掉电：对页重写未完成即断电 → init 取「尾 seq 最大且连续」页，
 * 原页仍完整在盘（压实先写后擦原页语义：原页不擦——翻页只擦未来的对页），
 * 最坏丢「压实开始后未及重写的 app 状态」（V1 如实接受，低频路径）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <ts/store.h>
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

/* 页扫描：最大连续 seq（audit 同型断尾语义）；返回该页是否有效数据 */
static bool page_scan(uint8_t page, uint32_t *max_seq, uint8_t *used)
{
	uint8_t slot[KV_SLOT_SZ];
	uint32_t prev = 0;

	*max_seq = 0;
	*used = 0;
	for (uint8_t i = 0; i < KV_SLOTS; i++) {
		uint32_t off = page * KV_PAGE_SZ + i * KV_SLOT_SZ;

		if (ts_store_backend.read(TS_PART_KV, off, slot, KV_SLOT_SZ) != TS_OK) {
			return false;
		}
		if (ts_get_le32(slot + KV_OFF_MAGIC) != KV_MAGIC) {
			break; /* 未写/半写 → 断尾 */
		}
		uint32_t seq = ts_get_le32(slot + KV_OFF_SEQ);

		if (seq == 0 || (i > 0 && seq != prev + 1)) {
			break; /* 序断裂（半写）→ 断尾 */
		}
		prev = seq;
		*max_seq = seq;
		(*used)++;
	}
	return *used > 0;
}

ts_res_t ts_store_kv_init(void)
{
	if (kv.ready) {
		return TS_OK;
	}
	if (ts_store_backend.size(TS_PART_KV) < KV_PAGES * KV_PAGE_SZ) {
		return TS_E_STATE; /* 分区未编入（CONFIG/DT 缺）——如实降级 */
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
	return TS_OK;
}

static void slot_encode(uint8_t *s, uint32_t hash, const uint8_t *data,
			uint16_t len, uint32_t seq)
{
	memset(s, 0xFF, KV_SLOT_SZ);
	ts_put_le32(s + KV_OFF_MAGIC, KV_MAGIC);
	ts_put_le32(s + KV_OFF_HASH, hash);
	ts_put_le32(s + KV_OFF_SEQ, seq);
	ts_put_le16(s + KV_OFF_LEN, len);
	memcpy(s + KV_OFF_DATA, data, len);
}

/* 两页收集「每 app 最新槽」；容量上限 = KV_SLOTS-1（**为随后的新写保
 * 至少一槽余量**——否则 15 个不同 app 压实后仍满页，新 save 无槽可写；
 * 超出按 seq 弃最旧 app——有界语义，V0.4.0 如实登记）。 */
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

			if (ts_store_backend.read(TS_PART_KV, off, slot, KV_SLOT_SZ) != TS_OK ||
			    ts_get_le32(slot + KV_OFF_MAGIC) != KV_MAGIC) {
				break;
			}
			uint32_t hash = ts_get_le32(slot + KV_OFF_HASH);
			uint32_t seq = ts_get_le32(slot + KV_OFF_SEQ);
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
					continue; /* 已有更新 */
				}
			} else if (n == KV_KEEP_MAX) {
				/* 容量满：替换 seq 最小的 app（弃最旧——保新写余量） */
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
			refs[hit].len = ts_get_le16(slot + KV_OFF_LEN);
			refs[hit].page = p;
			refs[hit].slot = i;
		}
	}
	return n;
}

int32_t ts_store_kv_load(uint32_t app_hash, uint8_t *out, uint16_t max)
{
	if (ts_store_kv_init() != TS_OK) {
		return TS_E_STATE;
	}
	uint8_t slot[KV_SLOT_SZ];
	int32_t best = -1;
	uint32_t best_seq = 0;
	uint16_t best_len = 0;

	for (uint8_t p = 0; p < KV_PAGES; p++) {
		for (uint8_t i = 0; i < KV_SLOTS; i++) {
			uint32_t off = p * KV_PAGE_SZ + i * KV_SLOT_SZ;

			if (ts_store_backend.read(TS_PART_KV, off, slot, KV_SLOT_SZ) != TS_OK ||
			    ts_get_le32(slot + KV_OFF_MAGIC) != KV_MAGIC) {
				break;
			}
			if (ts_get_le32(slot + KV_OFF_HASH) != app_hash) {
				continue;
			}
			uint32_t seq = ts_get_le32(slot + KV_OFF_SEQ);

			if (best < 0 || seq > best_seq) {
				best = (int32_t)i;
				best_seq = seq;
				best_len = ts_get_le16(slot + KV_OFF_LEN);
				if (best_len > max) {
					return TS_E_RANGE; /* 容量不足如实（先于覆盖） */
				}
				memcpy(out, slot + KV_OFF_DATA, best_len);
			}
		}
	}
	return (best < 0) ? TS_E_NOTFOUND : (int32_t)best_len;
}

ts_res_t ts_store_kv_save(uint32_t app_hash, const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0 || len > TS_KV_BLOB_MAX) {
		return TS_E_PARAM;
	}
	if (ts_store_kv_init() != TS_OK) {
		return TS_E_STATE;
	}
	if (kv.next_slot == KV_SLOTS) {
		/* 页满：压实翻页——每 app 最新槽重写至对页（先写后切，原页
		 * 不擦——压实窗口掉电的原页保全语义，见文件头）。 */
		static struct kv_ref refs[KV_KEEP_MAX];
		size_t n = collect_latest(refs);
		uint8_t target = kv.active_page ^ 1U;

		if (ts_store_backend.erase_off(TS_PART_KV, target * KV_PAGE_SZ,
					       KV_PAGE_SZ) != TS_OK) {
			return TS_E_IO;
		}
		for (size_t i = 0; i < n; i++) {
			/* 流式重写：按槽位回读源记录（DRAM 纪律——不物化 blob 集） */
			uint8_t slot[KV_SLOT_SZ];
			uint32_t src = refs[i].page * KV_PAGE_SZ + refs[i].slot * KV_SLOT_SZ;

			if (ts_store_backend.read(TS_PART_KV, src, slot, KV_SLOT_SZ) != TS_OK) {
				return TS_E_IO;
			}
			/* seq 重排（目标页连续递增——断尾扫描语义保持） */
			ts_put_le32(slot + KV_OFF_SEQ, kv.next_seq);
			if (ts_store_backend.write(TS_PART_KV,
						   target * KV_PAGE_SZ + i * KV_SLOT_SZ,
						   slot, KV_SLOT_SZ) != TS_OK) {
				return TS_E_IO;
			}
			kv.next_seq++;
		}
		kv.active_page = target;
		kv.next_slot = (uint8_t)n;
	}
	uint8_t slot[KV_SLOT_SZ];

	slot_encode(slot, app_hash, data, len, kv.next_seq);
	if (ts_store_backend.write(TS_PART_KV,
				   kv.active_page * KV_PAGE_SZ +
				   kv.next_slot * KV_SLOT_SZ,
				   slot, KV_SLOT_SZ) != TS_OK) {
		return TS_E_IO;
	}
	kv.next_seq++;
	kv.next_slot++;
	return TS_OK;
}

#ifdef CONFIG_TS_TEST
void ts_store_kv_test_reset(void)
{
	/* 测试隔离：后端抹除后游标失效——置 not ready（下次操作重扫描） */
	memset(&kv, 0, sizeof(kv));
}
#endif
