/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PACKAGE_H
#define ENTELEA_PACKAGE_H

/* R05 package container contract: see framework/services/package/contract.md.
 * TSAP v2 container (byte format identical to the legacy version, which is the
 * precondition for the comparison oracle; renaming to entelea_* does not
 * change the wire format). The header is 16B (big-endian) plus a 32B digest:
 *   magic "TSAP"(4) | fmt_ver u16(=2) | manifest_len u32 | wasm_len u32
 *   | flags u16 (bit0 = 32B sha256(manifest||wasm) follows the header;
 *     unknown bits fail closed)
 *   | digest(32) | manifest(CBOR) || wasm || COSE_Sign1 (ed25519, with an
 *     embedded payload copy; firmware checks that the two copies match, which
 *     prevents signing A but installing B).
 * v1 (fmt_ver=1) is rejected fail-closed (no compatible loading). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_TSAP_MAGIC 0x54534150U /* 'T','S','A','P' (big-endian) */
#define ENTELEA_TSAP_FMT_VER 2U
#define ENTELEA_TSAP_FLAGS_DIGEST 0x0001U
#define ENTELEA_TSAP_HEADER_SIZE 16U
#define ENTELEA_TSAP_DIGEST_SIZE 32U
#define ENTELEA_TSAP_DIGEST_OFF 16U
#define ENTELEA_TSAP_CONTENT_OFF \
	(ENTELEA_TSAP_HEADER_SIZE + ENTELEA_TSAP_DIGEST_SIZE)

typedef struct {
	uint16_t fmt_ver;
	uint16_t flags;
	uint32_t manifest_len;
	uint32_t wasm_len;
	uint32_t manifest_off;
	uint32_t wasm_off;
	uint32_t cose_off;
	uint32_t
		pkg_len; /* pkg length = end of COSE sig; 0 if unverified */
} entelea_tsap_view_t;

static inline uint32_t
entelea_be32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
	       ((uint32_t) p[2] << 8) | p[3];
}

static inline uint16_t
entelea_be16(const uint8_t *p)
{
	return (uint16_t) ((uint16_t) p[0] << 8 | p[1]);
}

/* Header field parsing (exact match on v2 and flags). Only header fields are
 * validated here; the caller verifies container bounds against the storage
 * surface separately. Overflow defense: segment layout must fit in u32. */
static inline bool
entelea_tsap_parse_fields(const uint8_t *hdr, entelea_tsap_view_t *v)
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
		return false; /* v1 or unknown flags = fail-closed */
	}
	v->fmt_ver = ver;
	v->flags = flags;
	v->manifest_len = entelea_be32(hdr + 6);
	v->wasm_len = entelea_be32(hdr + 10);
	if (v->manifest_len == 0 || v->wasm_len == 0) {
		return false;
	}
	v->pkg_len = 0;
	v->manifest_off = ENTELEA_TSAP_CONTENT_OFF;
	v->wasm_off = ENTELEA_TSAP_CONTENT_OFF + v->manifest_len;
	v->cose_off = v->wasm_off + v->wasm_len;
	/* review P1 fix: wasm_off itself must not wrap around or fall inside
	 * the manifest segment. Previously only cose_off was checked, so an
	 * oversized manifest_len could wrap wasm_off below 48, letting the
	 * executed bytes include an unsigned prefix. */
	if (v->wasm_off < v->manifest_off || v->cose_off < v->wasm_off ||
	    v->cose_off < ENTELEA_TSAP_CONTENT_OFF) {
		return false;
	}
	return true;
}

/* Parses a whole buffer (fields and container bounds). */
static inline bool
entelea_tsap_parse(const uint8_t *buf, size_t len, entelea_tsap_view_t *v)
{
	if (buf == NULL || len < ENTELEA_TSAP_HEADER_SIZE) {
		return false;
	}
	if (!entelea_tsap_parse_fields(buf, v)) {
		return false;
	}
	uint64_t total = (uint64_t) ENTELEA_TSAP_CONTENT_OFF + v->manifest_len +
			 v->wasm_len;

	return total + 1 <= (uint64_t) len && total <= (uint64_t) UINT32_MAX;
}

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PACKAGE_H */
