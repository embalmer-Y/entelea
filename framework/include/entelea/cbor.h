/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_CBOR_H
#define ENTELEA_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const uint8_t *p;
	size_t rem;
} entelea_cbor_reader_t;

bool entelea_cbor_init(entelea_cbor_reader_t *r, const uint8_t *buf, size_t len);
bool entelea_cbor_map_open(entelea_cbor_reader_t *r, uint32_t *pairs);
bool entelea_cbor_array_open(entelea_cbor_reader_t *r, uint32_t *items);
bool entelea_cbor_tstr(entelea_cbor_reader_t *r, char *out, size_t cap);
bool entelea_cbor_bstr_ref(entelea_cbor_reader_t *r, const uint8_t **out, uint32_t *len);
bool entelea_cbor_uint(entelea_cbor_reader_t *r, uint64_t *out);
bool entelea_cbor_int(entelea_cbor_reader_t *r, int64_t *out);
bool entelea_cbor_bool(entelea_cbor_reader_t *r, bool *out);
bool entelea_cbor_skip(entelea_cbor_reader_t *r, int depth);
bool entelea_cbor_put_map(uint8_t *buf, size_t cap, size_t *pos, uint32_t pairs);
bool entelea_cbor_put_array(uint8_t *buf, size_t cap, size_t *pos, uint32_t items);
bool entelea_cbor_put_tstrn(uint8_t *buf, size_t cap, size_t *pos, const char *s, size_t n);
bool entelea_cbor_put_tstr(uint8_t *buf, size_t cap, size_t *pos, const char *s);
bool entelea_cbor_put_bstr(uint8_t *buf, size_t cap, size_t *pos, const uint8_t *b, uint32_t n);
bool entelea_cbor_put_uint(uint8_t *buf, size_t cap, size_t *pos, uint64_t v);
bool entelea_cbor_put_int(uint8_t *buf, size_t cap, size_t *pos, int64_t v);
bool entelea_cbor_put_bool(uint8_t *buf, size_t cap, size_t *pos, bool v);

#ifdef __cplusplus
}
#endif
#endif
