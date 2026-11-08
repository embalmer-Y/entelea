/* SPDX-License-Identifier: Apache-2.0 */
/* entelea storage 内部共享（不对外）。 */
#ifndef ENTELEA_STORAGE_INTERNAL_H
#define ENTELEA_STORAGE_INTERNAL_H

#include <string.h>
#include <entelea/storage.h>

/* CRC16-CCITT（poly 0x1021，init 0xFFFF）——meta/noinit 校验。 */
static inline uint16_t entelea_crc16(const uint8_t *data, size_t len)
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

/* 后端 ops（part.c 实现；上层不感知后端）。 */
typedef struct {
	entelea_result_t (*read)(entelea_store_part_t part, uint32_t off, void *buf,
				 uint32_t len);
	entelea_result_t (*write)(entelea_store_part_t part, uint32_t off,
				  const void *buf, uint32_t len);
	entelea_result_t (*erase)(entelea_store_part_t part);
	entelea_result_t (*erase_off)(entelea_store_part_t part, uint32_t off,
				       uint32_t len);
	uint32_t (*size)(entelea_store_part_t part);
} entelea_store_ops_t;

extern const entelea_store_ops_t entelea_store_backend;

/* 小端读写（内部分区本机序）。 */
static inline void entelea_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void entelea_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t entelea_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t entelea_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

#endif /* ENTELEA_STORAGE_INTERNAL_H */
