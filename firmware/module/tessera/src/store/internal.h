/* SPDX-License-Identifier: Apache-2.0 */
/* ts-store 内部共享（不对外）。 */
#ifndef TS_STORE_INTERNAL_H__
#define TS_STORE_INTERNAL_H__

#include <stdint.h>
#include <ts/store.h>

/* CRC16-CCITT（poly 0x1021，init 0xFFFF）——meta/prov/noinit 校验（LLD-ts-store §3） */
static inline uint16_t ts_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

/* 后端 ops（part.c 实现；上层不感知后端） */
typedef struct {
	ts_res_t (*read)(ts_store_part_t part, uint32_t off, void *buf, uint32_t len);
	ts_res_t (*write)(ts_store_part_t part, uint32_t off, const void *buf, uint32_t len);
	ts_res_t (*erase)(ts_store_part_t part); /* 全区抹除 */
	ts_res_t (*erase_off)(ts_store_part_t part, uint32_t off, uint32_t len); /* 范围抹除（flash 后端按擦除块对齐；meta 副本步距用） */
	uint32_t (*size)(ts_store_part_t part);
} ts_store_ops_t;

extern const ts_store_ops_t ts_store_backend;

/* 自包含 SHA-256（sha256.c；slot_hash 流式摘要） */
typedef struct {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	uint32_t buf_len;
} ts_sha256_ctx_t;

void ts_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void ts_sha256_init(ts_sha256_ctx_t *ctx);
void ts_sha256_update(ts_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void ts_sha256_final(ts_sha256_ctx_t *ctx, uint8_t out[32]);

/* 小端读写（内部分区本机序） */
static inline void ts_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
static inline void ts_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t ts_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t ts_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- 定长 schema 确定性读取助手（prov.c/trust.c 共用——prov.c 首版提炼；
 * definite-length 专用，任何超集/不定长 = false——不引入通用 CBOR 依赖） */

typedef struct {
	const uint8_t *p;
	uint32_t rem;
} ts_rd_t;

static inline bool ts_rd_u8(ts_rd_t *r, uint8_t *out)
{
	if (r->rem < 1) {
		return false;
	}
	*out = *r->p++;
	r->rem--;
	return true;
}

static inline bool ts_rd_len(ts_rd_t *r, uint8_t info, uint32_t *out)
{
	if (info < 0x18) {
		*out = info;
		return true;
	}
	uint32_t n = 0;
	int bytes = (info == 0x18) ? 1 : (info == 0x19) ? 2 : (info == 0x1A) ? 4 : -1;

	if (bytes < 0 || r->rem < (uint32_t)bytes) {
		return false;
	}
	for (int i = 0; i < bytes; i++) {
		n = (n << 8) | *r->p++;
	}
	r->rem -= bytes;
	*out = n;
	return true;
}

static inline bool ts_rd_tstr(ts_rd_t *r, char *out, size_t cap)
{
	uint8_t ib;

	if (!ts_rd_u8(r, &ib) || (uint8_t)(ib & 0xE0) != 0x60) {
		return false;
	}
	uint32_t len;

	if (!ts_rd_len(r, ib & 0x1F, &len) || len >= cap || r->rem < len) {
		return false;
	}
	memcpy(out, r->p, len);
	out[len] = '\0';
	r->p += len;
	r->rem -= len;
	return true;
}

static inline bool ts_rd_bstr(ts_rd_t *r, uint8_t *out, size_t cap)
{
	uint8_t ib;

	if (!ts_rd_u8(r, &ib) || (uint8_t)(ib & 0xE0) != 0x40) {
		return false;
	}
	uint32_t len;

	if (!ts_rd_len(r, ib & 0x1F, &len) || len != cap || r->rem < len) {
		return false;
	}
	memcpy(out, r->p, len);
	r->p += len;
	r->rem -= len;
	return true;
}

/* 变长 bstr（prov v2 TLS 三元组——DEC-63③）：len ≤ cap 实长出参。
 * out=NULL = 仅消费走查（validate 通道零拷贝）。 */
static inline bool ts_rd_bstr_len(ts_rd_t *r, uint8_t *out, size_t cap,
				  uint16_t *len_out)
{
	uint8_t ib;

	if (!ts_rd_u8(r, &ib) || (uint8_t)(ib & 0xE0) != 0x40) {
		return false;
	}
	uint32_t len;

	if (!ts_rd_len(r, ib & 0x1F, &len) || len > cap || r->rem < len) {
		return false;
	}
	if (out != NULL) {
		memcpy(out, r->p, len);
	}
	*len_out = (uint16_t)len;
	r->p += len;
	r->rem -= len;
	return true;
}

static inline bool ts_rd_uint(ts_rd_t *r, uint32_t *out)
{
	uint8_t ib;

	if (!ts_rd_u8(r, &ib) || (ib & 0xE0) != 0x00) {
		return false;
	}
	uint32_t v;

	if (!ts_rd_len(r, ib & 0x1F, &v)) {
		return false;
	}
	*out = v;
	return true;
}

#endif /* TS_STORE_INTERNAL_H__ */
