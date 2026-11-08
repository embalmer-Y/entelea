/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_SHA256_H
#define ENTELEA_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	uint32_t buf_len;
} entelea_sha256_ctx_t;

entelea_result_t entelea_sha256_init(entelea_sha256_ctx_t *ctx);
entelea_result_t entelea_sha256_update(entelea_sha256_ctx_t *ctx,
	const uint8_t *data, size_t len);
entelea_result_t entelea_sha256_final(entelea_sha256_ctx_t *ctx, uint8_t out[32]);
entelea_result_t entelea_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
