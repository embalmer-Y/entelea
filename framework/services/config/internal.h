/* SPDX-License-Identifier: Apache-2.0 */
/* entelea config 内部共享（不对外）：定长 schema 确定性读取助手
 * （definite-length 专用；任何超集/不定长 = false——不引入通用 CBOR
 * 依赖，真机足迹纪律）+ CRC16。 */
#ifndef ENTELEA_CONFIG_INTERNAL_H
#define ENTELEA_CONFIG_INTERNAL_H

#include <string.h>
#include <entelea/prov.h>

static inline uint16_t entelea_cfg_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000U)
				      ? (uint16_t)((crc << 1) ^ 0x1021U)
				      : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static inline void entelea_cfg_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void entelea_cfg_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t entelea_cfg_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t entelea_cfg_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

typedef struct {
	const uint8_t *p;
	uint32_t rem;
} entelea_rd_t;

static inline bool entelea_rd_u8(entelea_rd_t *r, uint8_t *out)
{
	if (r->rem < 1) {
		return false;
	}
	*out = *r->p++;
	r->rem--;
	return true;
}

static inline bool entelea_rd_len(entelea_rd_t *r, uint8_t info, uint32_t *out)
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

static inline bool entelea_rd_tstr(entelea_rd_t *r, char *out, size_t cap)
{
	uint8_t ib;

	if (!entelea_rd_u8(r, &ib) || (uint8_t)(ib & 0xE0) != 0x60) {
		return false;
	}
	uint32_t len;

	if (!entelea_rd_len(r, ib & 0x1F, &len) || len >= cap || r->rem < len) {
		return false;
	}
	memcpy(out, r->p, len);
	out[len] = '\0';
	r->p += len;
	r->rem -= len;
	return true;
}

static inline bool entelea_rd_bstr(entelea_rd_t *r, uint8_t *out, size_t cap)
{
	uint8_t ib;

	if (!entelea_rd_u8(r, &ib) || (uint8_t)(ib & 0xE0) != 0x40) {
		return false;
	}
	uint32_t len;

	if (!entelea_rd_len(r, ib & 0x1F, &len) || len != cap || r->rem < len) {
		return false;
	}
	memcpy(out, r->p, len);
	r->p += len;
	r->rem -= len;
	return true;
}

static inline bool entelea_rd_bstr_len(entelea_rd_t *r, uint8_t *out, size_t cap,
				       uint16_t *len_out)
{
	uint8_t ib;

	if (!entelea_rd_u8(r, &ib) || (uint8_t)(ib & 0xE0) != 0x40) {
		return false;
	}
	uint32_t len;

	if (!entelea_rd_len(r, ib & 0x1F, &len) || len > cap || r->rem < len) {
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

static inline bool entelea_rd_uint(entelea_rd_t *r, uint32_t *out)
{
	uint8_t ib;

	if (!entelea_rd_u8(r, &ib) || (ib & 0xE0) != 0x00) {
		return false;
	}
	uint32_t v;

	if (!entelea_rd_len(r, ib & 0x1F, &v)) {
		return false;
	}
	*out = v;
	return true;
}

#endif /* ENTELEA_CONFIG_INTERNAL_H */
