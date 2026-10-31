/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 信任根表持久化（V0.8.0 EC2 · Q-43③/DEC-60；LLD-ts-store §4a）。
 * TUF root 同型全量替换表：map{"ver":uint, "keys":[bstr(32)×0..8]}——
 * 撤销 = 次版剔除（无独立 revoked 列表：定长撤销表溢出 = 旧键复活面，
 * 全量替换根除）。分区 = 8KB 双副本乒乓（2×4KB——擦除块对齐，
 * meta/audit/kv 同型约束；记录 seq/len/crc16 + CBOR——
 * §3 meta 同型撕裂安全：擦-写间掉电 → 旧副本完整）。
 * 职责分层（prov_maint 同型）：本层只管持久化与 schema 确定性走查；
 * 签名/单调 ver 门控在 ts-appmgr trust_update 编排链（verify.c）。
 * 惰性装载：首访只读装载（无写回面——get_info 惰性初始化缺陷不同型）。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <ts/store.h>
#include "internal.h"

#define TRUST_REC_HDR 8 /* seq u32 | len u16 | crc16 u16（§3 同型） */

static ts_trust_table_t table;
static bool loaded;

static uint32_t trust_stride(void)
{
	return ts_store_backend.size(TS_PART_TRUST) / 2;
}

static ts_res_t read_rec(uint8_t copy, uint32_t *seq, uint16_t *len, uint8_t *data)
{
	uint8_t hdr[TRUST_REC_HDR];
	uint32_t stride = trust_stride();

	if (stride < TRUST_REC_HDR + CONFIG_TS_TRUST_TABLE_MAX ||
	    ts_store_backend.read(TS_PART_TRUST, copy * stride, hdr,
				  TRUST_REC_HDR) != TS_OK) {
		return TS_E_IO;
	}
	*seq = ts_get_le32(hdr);
	*len = ts_get_le16(hdr + 4);
	uint16_t crc = ts_get_le16(hdr + 6);

	if (*len == 0 || *len > CONFIG_TS_TRUST_TABLE_MAX) {
		return TS_E_IO;
	}
	if (ts_store_backend.read(TS_PART_TRUST, copy * stride + TRUST_REC_HDR,
				  data, *len) != TS_OK) {
		return TS_E_IO;
	}
	if (ts_crc16(data, *len) != crc) {
		return TS_E_IO;
	}
	return TS_OK;
}

static ts_res_t write_rec(uint8_t copy, uint32_t seq, const void *buf, uint16_t len)
{
	uint8_t rec[TRUST_REC_HDR + CONFIG_TS_TRUST_TABLE_MAX];
	uint32_t stride = trust_stride();

	if (stride < TRUST_REC_HDR + len) {
		return TS_E_RANGE;
	}
	ts_put_le32(rec, seq);
	ts_put_le16(rec + 4, len);
	ts_put_le16(rec + 6, ts_crc16(buf, len));
	memcpy(rec + TRUST_REC_HDR, buf, len);
	uint32_t total = TRUST_REC_HDR + len;

	/* 撕裂时序同 meta §3：写前范围抹除目标副本 → 擦-写间掉电 → 该副本
	 * 损，另一副本仍完整。 */
	if (ts_store_backend.erase_off(TS_PART_TRUST, copy * stride, stride) != TS_OK ||
	    ts_store_backend.write(TS_PART_TRUST, copy * stride, rec, total) != TS_OK) {
		return TS_E_IO;
	}
	uint8_t back[TRUST_REC_HDR + CONFIG_TS_TRUST_TABLE_MAX];

	if (ts_store_backend.read(TS_PART_TRUST, copy * stride, back, total) != TS_OK ||
	    memcmp(rec, back, total) != 0) {
		return TS_E_IO; /* 回读校验 */
	}
	return TS_OK;
}

/* 确定性 schema 走查（validate/load 共用——单一事实源；prov_walk 同型） */
static ts_res_t trust_walk(const uint8_t *cbor, uint32_t cbor_len, ts_trust_table_t *out)
{
	ts_rd_t r = {.p = cbor, .rem = cbor_len};
	uint8_t ib;

	if (!ts_rd_u8(&r, &ib) || (uint8_t)(ib & 0xE0) != 0xA0) {
		return TS_E_IO;
	}
	uint32_t pairs;

	if (!ts_rd_len(&r, ib & 0x1F, &pairs) || pairs != 2) {
		return TS_E_IO; /* 恰 2 对定序键（超集/子集 = 结构损坏） */
	}
	char key[8];
	uint32_t u;

	/* 键序固定：ver → keys */
	if (!ts_rd_tstr(&r, key, sizeof(key)) || strcmp(key, "ver") != 0 ||
	    !ts_rd_uint(&r, &u) || u == 0) {
		return TS_E_IO; /* ver ≥ 1（ver 0 = 无记录哨兵，非合法表值） */
	}
	out->ver = u;
	if (!ts_rd_tstr(&r, key, sizeof(key)) || strcmp(key, "keys") != 0) {
		return TS_E_IO;
	}
	uint8_t ab;

	if (!ts_rd_u8(&r, &ab) || (uint8_t)(ab & 0xE0) != 0x80) {
		return TS_E_IO; /* definite array（不定长 = 拒） */
	}
	uint32_t n;

	if (!ts_rd_len(&r, ab & 0x1F, &n) || n > TS_TRUST_KEYS_MAX) {
		return TS_E_IO;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (!ts_rd_bstr(&r, out->keys[i], 32)) {
			return TS_E_IO;
		}
		/* 键内重复 = 拒（全量替换语义下重复条目为生成侧错误） */
		for (uint32_t j = 0; j < i; j++) {
			if (memcmp(out->keys[i], out->keys[j], 32) == 0) {
				return TS_E_IO;
			}
		}
	}
	if (r.rem != 0) {
		return TS_E_IO; /* 尾部多余字节 = 结构损坏 */
	}
	out->n_keys = (uint8_t)n;
	return TS_OK;
}

ts_res_t ts_store_trust_validate(const uint8_t *cbor, uint32_t len, ts_trust_table_t *out)
{
	if (cbor == NULL || len == 0 || len > CONFIG_TS_TRUST_TABLE_MAX) {
		return TS_E_PARAM;
	}
	return trust_walk(cbor, len, out);
}

ts_res_t ts_store_trust_load(void)
{
	static uint8_t data[CONFIG_TS_TRUST_TABLE_MAX];
	uint32_t s0, s1;
	uint16_t l0, l1;

	bool ok0 = read_rec(0, &s0, &l0, data) == TS_OK;
	ts_trust_table_t t0;

	if (ok0 && trust_walk(data, l0, &t0) != TS_OK) {
		ok0 = false; /* CRC 过而 schema 违例 = 损（防生成侧坏表落位） */
	}
	bool ok1 = read_rec(1, &s1, &l1, data) == TS_OK;
	ts_trust_table_t t1;

	if (ok1 && trust_walk(data, l1, &t1) != TS_OK) {
		ok1 = false;
	}
	if (ok0 && ok1) {
		/* seq 较大者活动（同 seq = copy0——写序恒 N+1 不会同 seq 并存双 CRC 过） */
		table = (s0 >= s1) ? t0 : t1;
	} else if (ok0) {
		table = t0;
	} else if (ok1) {
		table = t1;
	} else {
		/* 双损/空分区 = 工厂根单源（fail-safe 侧——首次注册 ver=1 起链） */
		memset(&table, 0, sizeof(table));
	}
	loaded = true;
	return TS_OK;
}

const ts_trust_table_t *ts_store_trust(void)
{
	if (!loaded) {
		(void)ts_store_trust_load();
	}
	return &table;
}

ts_res_t ts_store_trust_write(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len > CONFIG_TS_TRUST_TABLE_MAX) {
		return TS_E_PARAM;
	}
	ts_trust_table_t t;

	ts_res_t v = trust_walk(cbor, len, &t);

	if (v != TS_OK) {
		return v; /* 非法表 = 零写入（fail-closed） */
	}
	uint32_t s0, s1;
	uint16_t l0, l1;
	uint8_t data[CONFIG_TS_TRUST_TABLE_MAX];

	bool ok0 = read_rec(0, &s0, &l0, data) == TS_OK;
	bool ok1 = read_rec(1, &s1, &l1, data) == TS_OK;

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
		return TS_E_RANGE; /* seq 回绕（结构性不可能——防御） */
	}
	ts_res_t r = write_rec(inactive, cur_seq + 1, cbor, (uint16_t)len);

	if (r != TS_OK) {
		return r;
	}
	return ts_store_trust_load(); /* 活动副本切换 + RAM 表重载 */
}

#ifdef CONFIG_TS_TEST
void ts_store_trust_test_reset(void)
{
	loaded = false;
	memset(&table, 0, sizeof(table));
}

ts_res_t ts_store_trust_corrupt_test(uint8_t copy_idx)
{
	uint32_t stride = trust_stride();

	if (copy_idx > 1 || stride < TRUST_REC_HDR) {
		return TS_E_PARAM;
	}
	/* flash 后端物理只可 1→0：先范围抹除目标副本再写垃圾（meta_corrupt
	 * 同型——注入语义 = "该副本损毁"，len=0 使 read_rec 拒） */
	if (ts_store_backend.erase_off(TS_PART_TRUST, copy_idx * stride,
				       stride) != TS_OK) {
		return TS_E_IO;
	}
	uint8_t garbage[TRUST_REC_HDR] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};

	return ts_store_backend.write(TS_PART_TRUST, copy_idx * stride,
				      garbage, TRUST_REC_HDR);
}
#endif
