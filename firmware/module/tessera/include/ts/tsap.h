/* SPDX-License-Identifier: Apache-2.0 */
/*
 * TSAP v2 容器格式（DEC-21 v1 → v2 = DEC-49②；LLD-ts-appmgr §2）。
 * 头 16 字节（v1 原序）+ 32B 摘要（flags 位驱动）：
 *   magic "TSAP"(4) | fmt_ver u16(=2) | manifest_len u32 | wasm_len u32
 *   | flags u16（bit0=1：头后随 32B sha256(manifest‖wasm)；其余位保留 = 0，
 *     未知 flags = fail-closed 拒收）
 *   | digest(32) | manifest(CBOR) ‖ wasm ‖ COSE_Sign1(ed25519，payload =
 *     manifest‖wasm 内嵌副本——固件验签比对两副本一致，防"签 A 装 B"）。
 * **多字节整数一律大端**（网络序，与 CBOR/COSE 生态一致）。
 * v1 包（fmt_ver=1）v2 固件 fail-closed 拒收（DEC-49②：无兼容装载）。
 */
#ifndef TS_TSAP_H__
#define TS_TSAP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSAP_MAGIC         0x54534150U /* 字节序：'T','S','A','P'（大端读取） */
#define TSAP_FMT_VER       2U
#define TSAP_FLAGS_DIGEST  0x0001U
#define TSAP_HEADER_SIZE   16U
#define TSAP_DIGEST_SIZE   32U
#define TSAP_DIGEST_OFF    16U
#define TSAP_CONTENT_OFF   (TSAP_HEADER_SIZE + TSAP_DIGEST_SIZE) /* = 48 */

typedef struct {
	uint16_t fmt_ver;
	uint16_t flags;
	uint32_t manifest_len;
	uint32_t wasm_len;
	uint32_t manifest_off; /* = TSAP_CONTENT_OFF */
	uint32_t wasm_off;     /* = content + manifest_len */
	uint32_t cose_off;     /* = wasm_off + wasm_len */
} tsap_view_t;

static inline uint32_t tsap_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t tsap_be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/*
 * 解析 16B 头字段（v2 + flags 精确匹配）：只做头部域校验，容器边界由
 * 调用方按存储面（slot 尺寸/总长）另验——staging/boot 均自 slot 读，
 * 无整包缓冲可依。
 */
static inline bool tsap_header_parse_fields(const uint8_t *hdr, tsap_view_t *v)
{
	if (hdr == NULL || v == NULL) {
		return false;
	}
	if (tsap_be32(hdr) != TSAP_MAGIC) {
		return false;
	}
	uint16_t ver = tsap_be16(hdr + 4);
	uint16_t flags = tsap_be16(hdr + 14);

	if (ver != TSAP_FMT_VER || flags != TSAP_FLAGS_DIGEST) {
		return false; /* v1/未知 flags = fail-closed */
	}
	v->fmt_ver = ver;
	v->flags = flags;
	v->manifest_len = tsap_be32(hdr + 6);
	v->wasm_len = tsap_be32(hdr + 10);
	if (v->manifest_len == 0 || v->wasm_len == 0) {
		return false;
	}
	v->manifest_off = TSAP_CONTENT_OFF;
	v->wasm_off = TSAP_CONTENT_OFF + v->manifest_len;
	v->cose_off = v->wasm_off + v->wasm_len;
	/* 溢出防御：段布局必须落在 u32 域（调用方再对存储面上界收紧） */
	if (v->cose_off < v->wasm_off || v->cose_off < TSAP_CONTENT_OFF) {
		return false;
	}
	return true;
}

/* 整包缓冲解析（M2a store 测试沿用）：字段解析 + 容器边界（content+cose ≥1
 * 须在 len 内）。staging/boot 路径用 fields 变体（自 slot 读，无整包缓冲）。 */
static inline bool tsap_header_parse(const uint8_t *buf, size_t len, tsap_view_t *v)
{
	if (buf == NULL || len < TSAP_HEADER_SIZE) {
		return false;
	}
	if (!tsap_header_parse_fields(buf, v)) {
		return false;
	}
	uint64_t total = (uint64_t)TSAP_CONTENT_OFF + v->manifest_len + v->wasm_len;

	return total + 1 <= (uint64_t)len && total <= (uint64_t)UINT32_MAX;
}

#ifdef __cplusplus
}
#endif

#endif /* TS_TSAP_H__ */
