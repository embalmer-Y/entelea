/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TS_COMMON_COMPAT_H
#define TS_COMMON_COMPAT_H

#include <entelea/cbor.h>
#include <entelea/sha256.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef entelea_cbor_reader_t ts_cbor_rd_t;
typedef entelea_sha256_ctx_t ts_sha256_ctx_t;

void ts_cbor_rd_init(ts_cbor_rd_t *r, const uint8_t *buf, size_t len);
bool ts_cbor_map_open(ts_cbor_rd_t *r, uint32_t *pairs);
bool ts_cbor_array_open(ts_cbor_rd_t *r, uint32_t *items);
bool ts_cbor_tstr(ts_cbor_rd_t *r, char *out, size_t cap);
bool ts_cbor_bstr_ref(ts_cbor_rd_t *r, const uint8_t **out, uint32_t *len);
bool ts_cbor_uint(ts_cbor_rd_t *r, uint64_t *out);
bool ts_cbor_int(ts_cbor_rd_t *r, int64_t *out);
bool ts_cbor_bool(ts_cbor_rd_t *r, bool *out);
bool ts_cbor_skip(ts_cbor_rd_t *r, int depth);
bool ts_cbor_put_map(uint8_t *buf, size_t cap, size_t *pos, uint32_t pairs);
bool ts_cbor_put_array(uint8_t *buf, size_t cap, size_t *pos, uint32_t items);
bool ts_cbor_put_tstr(uint8_t *buf, size_t cap, size_t *pos, const char *s);
bool ts_cbor_put_tstrn(uint8_t *buf, size_t cap, size_t *pos, const char *s, size_t n);
bool ts_cbor_put_bstr(uint8_t *buf, size_t cap, size_t *pos, const uint8_t *s, uint32_t n);
bool ts_cbor_put_uint(uint8_t *buf, size_t cap, size_t *pos, uint64_t v);
bool ts_cbor_put_int(uint8_t *buf, size_t cap, size_t *pos, int64_t v);
bool ts_cbor_put_bool(uint8_t *buf, size_t cap, size_t *pos, bool v);
void ts_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void ts_sha256_init(ts_sha256_ctx_t *ctx);
void ts_sha256_update(ts_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void ts_sha256_final(ts_sha256_ctx_t *ctx, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
