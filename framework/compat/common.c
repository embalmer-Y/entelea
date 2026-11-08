/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <ts/common.h>

static void require_legacy(bool valid)
{
	if (!valid) {
		fputs("Entelea: legacy void API precondition violated\n", stderr);
		abort();
	}
}

void ts_cbor_rd_init(ts_cbor_rd_t *r, const uint8_t *buf, size_t len)
{
	require_legacy(entelea_cbor_init(r, buf, len));
}

bool ts_cbor_map_open(ts_cbor_rd_t *r, uint32_t *pairs)
{
	return entelea_cbor_map_open(r, pairs);
}

bool ts_cbor_array_open(ts_cbor_rd_t *r, uint32_t *items)
{
	return entelea_cbor_array_open(r, items);
}

bool ts_cbor_tstr(ts_cbor_rd_t *r, char *out, size_t cap)
{
	return entelea_cbor_tstr(r, out, cap);
}

bool ts_cbor_bstr_ref(ts_cbor_rd_t *r, const uint8_t **out, uint32_t *len)
{
	return entelea_cbor_bstr_ref(r, out, len);
}

bool ts_cbor_uint(ts_cbor_rd_t *r, uint64_t *out)
{
	return entelea_cbor_uint(r, out);
}

bool ts_cbor_int(ts_cbor_rd_t *r, int64_t *out)
{
	return entelea_cbor_int(r, out);
}

bool ts_cbor_bool(ts_cbor_rd_t *r, bool *out)
{
	return entelea_cbor_bool(r, out);
}

bool ts_cbor_skip(ts_cbor_rd_t *r, int depth)
{
	return entelea_cbor_skip(r, depth);
}

bool ts_cbor_put_map(uint8_t *buf, size_t cap, size_t *pos, uint32_t n)
{
	return entelea_cbor_put_map(buf, cap, pos, n);
}

bool ts_cbor_put_array(uint8_t *buf, size_t cap, size_t *pos, uint32_t n)
{
	return entelea_cbor_put_array(buf, cap, pos, n);
}

bool ts_cbor_put_tstr(uint8_t *buf, size_t cap, size_t *pos, const char *s)
{
	return entelea_cbor_put_tstr(buf, cap, pos, s);
}

bool ts_cbor_put_tstrn(uint8_t *buf, size_t cap, size_t *pos, const char *s, size_t n)
{
	return entelea_cbor_put_tstrn(buf, cap, pos, s, n);
}

bool ts_cbor_put_bstr(uint8_t *buf, size_t cap, size_t *pos, const uint8_t *s, uint32_t n)
{
	return entelea_cbor_put_bstr(buf, cap, pos, s, n);
}

bool ts_cbor_put_uint(uint8_t *buf, size_t cap, size_t *pos, uint64_t v)
{
	return entelea_cbor_put_uint(buf, cap, pos, v);
}

bool ts_cbor_put_int(uint8_t *buf, size_t cap, size_t *pos, int64_t v)
{
	return entelea_cbor_put_int(buf, cap, pos, v);
}

bool ts_cbor_put_bool(uint8_t *buf, size_t cap, size_t *pos, bool v)
{
	return entelea_cbor_put_bool(buf, cap, pos, v);
}

void ts_sha256_init(ts_sha256_ctx_t *ctx)
{
	require_legacy(entelea_sha256_init(ctx) == ENTELEA_OK);
}

void ts_sha256_update(ts_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
	require_legacy(entelea_sha256_update(ctx, data, len) == ENTELEA_OK);
}

void ts_sha256_final(ts_sha256_ctx_t *ctx, uint8_t out[32])
{
	require_legacy(entelea_sha256_final(ctx, out) == ENTELEA_OK);
}

void ts_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	require_legacy(entelea_sha256(data, len, out) == ENTELEA_OK);
}
