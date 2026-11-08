/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 信任根表持久化（迁自旧 store/ts_trust.c，R03 合同见本目录 contract.md）。
 * 分区 = 8KB 双副本乒乓（2×4KB，擦除块对齐，meta 同型撕裂安全）。
 */
#include <string.h>
#include "config_reader.h"
#include <entelea/storage.h>
#include <entelea/trust.h>
#include "internal.h"

#define TRUST_REC_HDR 8 /* seq u32 | len u16 | crc16 u16（meta 同型） */
#define TRUST_BACKEND_SIZE 8192U

static entelea_trust_table_t table;
static bool loaded;

static uint32_t trust_stride(void)
{
	return TRUST_BACKEND_SIZE / 2;
}

static entelea_result_t read_rec(uint8_t copy, uint32_t *seq, uint16_t *len,
				 uint8_t *data)
{
	uint8_t hdr[TRUST_REC_HDR];
	uint32_t stride = trust_stride();

	if (stride < TRUST_REC_HDR + ENTELEA_TRUST_CBOR_MAX ||
	    entelea_store_backend.read(ENTELEA_PART_TRUST, copy * stride, hdr,
				       TRUST_REC_HDR) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	*seq = entelea_get_le32(hdr);
	*len = entelea_get_le16(hdr + 4);
	uint16_t crc = entelea_get_le16(hdr + 6);

	if (*len == 0 || *len > ENTELEA_TRUST_CBOR_MAX) {
		return ENTELEA_E_IO;
	}
	if (entelea_store_backend.read(ENTELEA_PART_TRUST, copy * stride + TRUST_REC_HDR,
				       data, *len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_crc16(data, *len) != crc) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

static entelea_result_t write_rec(uint8_t copy, uint32_t seq, const void *buf,
				  uint16_t len)
{
	uint8_t rec[TRUST_REC_HDR + ENTELEA_TRUST_CBOR_MAX];
	uint32_t stride = trust_stride();

	if (stride < TRUST_REC_HDR + len) {
		return ENTELEA_E_RANGE;
	}
	entelea_put_le32(rec, seq);
	entelea_put_le16(rec + 4, len);
	entelea_put_le16(rec + 6, entelea_crc16(buf, len));
	memcpy(rec + TRUST_REC_HDR, buf, len);
	uint32_t total = TRUST_REC_HDR + len;

	if (entelea_store_backend.erase_off(ENTELEA_PART_TRUST, copy * stride,
					    stride) != ENTELEA_OK ||
	    entelea_store_backend.write(ENTELEA_PART_TRUST, copy * stride, rec,
					total) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint8_t back[TRUST_REC_HDR + ENTELEA_TRUST_CBOR_MAX];

	if (entelea_store_backend.read(ENTELEA_PART_TRUST, copy * stride, back,
				       total) != ENTELEA_OK ||
	    memcmp(rec, back, total) != 0) {
		return ENTELEA_E_IO; /* 回读校验 */
	}
	return ENTELEA_OK;
}

/* 确定性 schema 走查（validate/load 共用——单一事实源）。 */
static entelea_result_t trust_walk(const uint8_t *cbor, uint32_t cbor_len,
				   entelea_trust_table_t *out)
{
	entelea_rd_t r = {.p = cbor, .rem = cbor_len};
	uint8_t ib;

	if (!entelea_rd_u8(&r, &ib) || (uint8_t)(ib & 0xE0) != 0xA0) {
		return ENTELEA_E_IO;
	}
	uint32_t pairs;

	if (!entelea_rd_len(&r, ib & 0x1F, &pairs) || pairs != 2) {
		return ENTELEA_E_IO; /* 恰 2 对定序键 */
	}
	char key[8];
	uint32_t u;

	if (!entelea_rd_tstr(&r, key, sizeof(key)) || strcmp(key, "ver") != 0 ||
	    !entelea_rd_uint(&r, &u) || u == 0) {
		return ENTELEA_E_IO; /* ver ≥ 1（0 = 哨兵非合法表值） */
	}
	out->ver = u;
	if (!entelea_rd_tstr(&r, key, sizeof(key)) || strcmp(key, "keys") != 0) {
		return ENTELEA_E_IO;
	}
	uint8_t ab;

	if (!entelea_rd_u8(&r, &ab) || (uint8_t)(ab & 0xE0) != 0x80) {
		return ENTELEA_E_IO; /* definite array */
	}
	uint32_t n;

	if (!entelea_rd_len(&r, ab & 0x1F, &n) || n > ENTELEA_TRUST_KEYS_MAX) {
		return ENTELEA_E_IO;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (!entelea_rd_bstr(&r, out->keys[i], 32)) {
			return ENTELEA_E_IO;
		}
		for (uint32_t j = 0; j < i; j++) {
			if (memcmp(out->keys[i], out->keys[j], 32) == 0) {
				return ENTELEA_E_IO; /* 键内重复 = 生成侧错误 */
			}
		}
	}
	if (r.rem != 0) {
		return ENTELEA_E_IO; /* 尾部余量 = 结构损坏 */
	}
	out->n_keys = (uint8_t)n;
	return ENTELEA_OK;
}

entelea_result_t entelea_trust_validate(const uint8_t *cbor, uint32_t len,
					entelea_trust_table_t *out)
{
	if (cbor == NULL || len == 0 || len > ENTELEA_TRUST_CBOR_MAX) {
		return ENTELEA_E_PARAM;
	}
	return trust_walk(cbor, len, out);
}

entelea_result_t entelea_trust_load(void)
{
	static uint8_t data[ENTELEA_TRUST_CBOR_MAX];
	uint32_t s0, s1;
	uint16_t l0, l1;

	bool ok0 = read_rec(0, &s0, &l0, data) == ENTELEA_OK;
	entelea_trust_table_t t0;

	if (ok0 && trust_walk(data, l0, &t0) != ENTELEA_OK) {
		ok0 = false; /* CRC 过而 schema 违例 = 损 */
	}
	bool ok1 = read_rec(1, &s1, &l1, data) == ENTELEA_OK;
	entelea_trust_table_t t1;

	if (ok1 && trust_walk(data, l1, &t1) != ENTELEA_OK) {
		ok1 = false;
	}
	if (ok0 && ok1) {
		table = (s0 >= s1) ? t0 : t1;
	} else if (ok0) {
		table = t0;
	} else if (ok1) {
		table = t1;
	} else {
		memset(&table, 0, sizeof(table)); /* 双损/空 = 工厂根单源 */
	}
	loaded = true;
	return ENTELEA_OK;
}

const entelea_trust_table_t *entelea_trust(void)
{
	if (!loaded) {
		(void)entelea_trust_load();
	}
	return &table;
}

entelea_result_t entelea_trust_write(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len > ENTELEA_TRUST_CBOR_MAX) {
		return ENTELEA_E_PARAM;
	}
	entelea_trust_table_t t;

	if (trust_walk(cbor, len, &t) != ENTELEA_OK) {
		return ENTELEA_E_IO; /* 非法=零写入（fail-closed） */
	}
	uint32_t s0, s1;
	uint16_t l0, l1;
	uint8_t data[ENTELEA_TRUST_CBOR_MAX];

	bool ok0 = read_rec(0, &s0, &l0, data) == ENTELEA_OK;
	bool ok1 = read_rec(1, &s1, &l1, data) == ENTELEA_OK;

	uint32_t cur_seq = 0;
	uint8_t inactive = 0;

	if (ok0 && ok1) {
		cur_seq = (s0 >= s1) ? s0 : s1;
		inactive = (s0 >= s1) ? 1 : 0;
	} else if (ok0) {
		cur_seq = s0;
		inactive = 1;
	} else if (ok1) {
		cur_seq = s1;
		inactive = 0;
	}
	if (cur_seq + 1 < cur_seq) {
		return ENTELEA_E_RANGE; /* seq 回绕（防御） */
	}
	entelea_result_t r = write_rec(inactive, cur_seq + 1, cbor, (uint16_t)len);

	if (r != ENTELEA_OK) {
		return r;
	}
	return entelea_trust_load(); /* 活动切换 + RAM 重载 */
}

#ifdef ENTELEA_TEST
void entelea_trust_test_reset(void)
{
	loaded = false;
	memset(&table, 0, sizeof(table));
}

entelea_result_t entelea_trust_corrupt_test(uint8_t copy_idx)
{
	uint32_t stride = trust_stride();

	if (copy_idx > 1 || stride < TRUST_REC_HDR) {
		return ENTELEA_E_PARAM;
	}
	if (entelea_store_backend.erase_off(ENTELEA_PART_TRUST, copy_idx * stride,
					    stride) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint8_t garbage[TRUST_REC_HDR] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};

	return entelea_store_backend.write(ENTELEA_PART_TRUST, copy_idx * stride,
					   garbage, TRUST_REC_HDR);
}
#endif
