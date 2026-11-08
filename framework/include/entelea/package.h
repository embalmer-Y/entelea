/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PACKAGE_H
#define ENTELEA_PACKAGE_H

/* R05 包容器合同见 framework/services/package/contract.md。
 * TSAP v2 容器（字节格式与旧版逐字节一致——对拍oracle 的前提；改名
 * entelea_* 不改线格式）。头 16B（大端）+ 32B 摘要：
 *   magic "TSAP"(4) | fmt_ver u16(=2) | manifest_len u32 | wasm_len u32
 *   | flags u16（bit0=头后随 32B sha256(manifest‖wasm)；未知位 fail-closed）
 *   | digest(32) | manifest(CBOR) ‖ wasm ‖ COSE_Sign1(ed25519，内嵌
 *     payload 副本——固件比对两副本一致，防"签 A 装 B"）。
 * v1（fmt_ver=1）fail-closed 拒收（无兼容装载）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_TSAP_MAGIC        0x54534150U /* 'T','S','A','P'（大端） */
#define ENTELEA_TSAP_FMT_VER      2U
#define ENTELEA_TSAP_FLAGS_DIGEST 0x0001U
#define ENTELEA_TSAP_HEADER_SIZE  16U
#define ENTELEA_TSAP_DIGEST_SIZE  32U
#define ENTELEA_TSAP_DIGEST_OFF   16U
#define ENTELEA_TSAP_CONTENT_OFF  (ENTELEA_TSAP_HEADER_SIZE + ENTELEA_TSAP_DIGEST_SIZE)

typedef struct {
	uint16_t fmt_ver;
	uint16_t flags;
	uint32_t manifest_len;
	uint32_t wasm_len;
	uint32_t manifest_off;
	uint32_t wasm_off;
	uint32_t cose_off;
} entelea_tsap_view_t;

static inline uint32_t entelea_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t entelea_be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* 头字段解析（v2+flags 精确匹配；仅头域校验，容器边界由调用方按存储面
 * 另验）。溢出防御：段布局须落 u32 域。 */
static inline bool entelea_tsap_parse_fields(const uint8_t *hdr,
					     entelea_tsap_view_t *v)
{
	if (hdr == NULL || v == NULL) {
		return false;
	}
	if (entelea_be32(hdr) != ENTELEA_TSAP_MAGIC) {
		return false;
	}
	uint16_t ver = entelea_be16(hdr + 4);
	uint16_t flags = entelea_be16(hdr + 14);

	if (ver != ENTELEA_TSAP_FMT_VER || flags != ENTELEA_TSAP_FLAGS_DIGEST) {
		return false; /* v1/未知 flags = fail-closed */
	}
	v->fmt_ver = ver;
	v->flags = flags;
	v->manifest_len = entelea_be32(hdr + 6);
	v->wasm_len = entelea_be32(hdr + 10);
	if (v->manifest_len == 0 || v->wasm_len == 0) {
		return false;
	}
	v->manifest_off = ENTELEA_TSAP_CONTENT_OFF;
	v->wasm_off = ENTELEA_TSAP_CONTENT_OFF + v->manifest_len;
	v->cose_off = v->wasm_off + v->wasm_len;
	/* review P1 修复：wasm_off 自身也须无回绕且不落在 manifest 段内
	 * （此前仅查 cose_off——超大 manifest_len 可使 wasm_off 回绕到
	 * 48 以下，执行字节可含未签名前缀）。 */
	if (v->wasm_off < v->manifest_off || v->cose_off < v->wasm_off ||
	    v->cose_off < ENTELEA_TSAP_CONTENT_OFF) {
		return false;
	}
	return true;
}

/* 整包缓冲解析（字段 + 容器边界）。 */
static inline bool entelea_tsap_parse(const uint8_t *buf, size_t len,
				      entelea_tsap_view_t *v)
{
	if (buf == NULL || len < ENTELEA_TSAP_HEADER_SIZE) {
		return false;
	}
	if (!entelea_tsap_parse_fields(buf, v)) {
		return false;
	}
	uint64_t total = (uint64_t)ENTELEA_TSAP_CONTENT_OFF + v->manifest_len +
			 v->wasm_len;

	return total + 1 <= (uint64_t)len && total <= (uint64_t)UINT32_MAX;
}

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PACKAGE_H */
