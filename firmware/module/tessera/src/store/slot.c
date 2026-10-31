/* SPDX-License-Identifier: Apache-2.0 */
/*
 * APP slot 读写（LLD-ts-store §6）：逐块写 + 回读校验；hash 供安装校验。
 * 消费者 ts-appmgr（M2b）：升级写序（inactive 写→校验→meta 原子切换）由其编排。
 */
#include <string.h>
#include <ts/store.h>
#include "internal.h"

#define SLOT_RW_CHUNK 256 /* 逐块回读校验粒度（结构性数值） */

static ts_res_t slot_of(uint8_t slot, ts_store_part_t *part)
{
	switch (slot) {
	case 0:
		*part = TS_PART_SLOT_A;
		return TS_OK;
	case 1:
		*part = TS_PART_SLOT_B;
		return TS_OK;
#if CONFIG_TS_STORE_SLOT_COUNT >= 3
	case 2:
		*part = TS_PART_SLOT_C;
		return TS_OK;
#endif
#if CONFIG_TS_STORE_SLOT_COUNT >= 4
	case 3:
		*part = TS_PART_SLOT_D;
		return TS_OK;
#endif
	default:
		return TS_E_PARAM;
	}
}

ts_res_t ts_store_slot_write(uint8_t slot, uint32_t off, const void *buf, uint32_t len)
{
	ts_store_part_t part;

	if (buf == NULL || slot_of(slot, &part) != TS_OK) {
		return TS_E_PARAM;
	}
	const uint8_t *src = buf;
	uint32_t size = ts_store_backend.size(part);

	if (off > size || len > size - off) { /* 溢出安全边界（IR-07） */
		return TS_E_RANGE;
	}
	for (uint32_t o = 0; o < len; o += SLOT_RW_CHUNK) {
		uint32_t n = len - o < SLOT_RW_CHUNK ? len - o : SLOT_RW_CHUNK;

		if (ts_store_backend.write(part, off + o, src + o, n) != TS_OK) {
			return TS_E_IO;
		}
		uint8_t back[SLOT_RW_CHUNK];

		if (ts_store_backend.read(part, off + o, back, n) != TS_OK ||
		    memcmp(back, src + o, n) != 0) {
			return TS_E_IO; /* 回读校验失败（LLD §6） */
		}
	}
	return TS_OK;
}

ts_res_t ts_store_slot_erase(uint8_t slot)
{
	ts_store_part_t part;

	if (slot_of(slot, &part) != TS_OK) {
		return TS_E_PARAM;
	}
	return ts_store_backend.erase(part);
}

ts_res_t ts_store_slot_read(uint8_t slot, uint32_t off, void *buf, uint32_t len)
{
	ts_store_part_t part;

	if (buf == NULL || slot_of(slot, &part) != TS_OK) {
		return TS_E_PARAM;
	}
	uint32_t size = ts_store_backend.size(part);

	if (off > size || len > size - off) { /* 溢出安全边界（IR-07） */
		return TS_E_RANGE;
	}
	return ts_store_backend.read(part, off, buf, len);
}

ts_res_t ts_store_slot_hash(uint8_t slot, uint8_t sha[32])
{
	ts_store_part_t part;

	if (sha == NULL || slot_of(slot, &part) != TS_OK) {
		return TS_E_PARAM;
	}
	ts_sha256_ctx_t ctx;
	uint32_t size = ts_store_backend.size(part);
	uint8_t chunk[64];

	ts_sha256_init(&ctx);
	for (uint32_t off = 0; off < size; off += sizeof(chunk)) {
		uint32_t n = size - off < sizeof(chunk) ? size - off : sizeof(chunk);

		if (ts_store_backend.read(part, off, chunk, n) != TS_OK) {
			return TS_E_IO;
		}
		ts_sha256_update(&ctx, chunk, n);
	}
	ts_sha256_final(&ctx, sha);
	return TS_OK;
}

/* ---- ts_store_init（LLD-ts-store §2）----------------------------------- */

ts_res_t ts_store_init(void)
{
	/* 后端为静态初始化（无动态 init）；此处执行 prov 加载与 noinit 读取。
	 * prov CRC/解析失败 → TS_E_IO，调用方（boot 编排，M2b 接线）进 fail-safe
	 *（合同 6）；noinit 结果经 ts_store_noinit_get 由消费者读取。 */
	return ts_store_prov_load();
}
