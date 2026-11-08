/* SPDX-License-Identifier: Apache-2.0 */
/* A-CBOR definite-length subset; transactional reads/writes on failure. */
#include <string.h>
#include <entelea/cbor.h>

bool entelea_cbor_init(entelea_cbor_reader_t *r, const uint8_t *buf, size_t len)
{
	if (r == NULL || (buf == NULL && len != 0)) {
		return false;
	}
	*r = (entelea_cbor_reader_t){.p = buf, .rem = len};
	return true;
}

static bool head(entelea_cbor_reader_t *r, uint8_t *major, uint64_t *value)
{
	if (r->p == NULL || r->rem == 0) {
		return false;
	}
	uint8_t first = *r->p++;
	uint8_t info = first & 31;

	r->rem--;
	*major = first >> 5;
	if (info < 24) {
		*value = info;
		return true;
	}
	int bytes = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : -1;

	if (bytes < 0 || r->rem < (size_t)bytes) {
		return false;
	}
	uint64_t v = 0;

	for (int i = 0; i < bytes; i++) {
		v = (v << 8) | *r->p++;
	}
	r->rem -= (size_t)bytes;
	*value = v;
	return true;
}

static bool container(entelea_cbor_reader_t *r, uint32_t *out, uint8_t kind,
	uint64_t limit)
{
	if (r == NULL || out == NULL) {
		return false;
	}
	entelea_cbor_reader_t next = *r;
	uint8_t major;
	uint64_t value;

	if (!head(&next, &major, &value) || major != kind || value > limit) {
		return false;
	}
	*out = (uint32_t)value;
	*r = next;
	return true;
}

bool entelea_cbor_map_open(entelea_cbor_reader_t *r, uint32_t *out)
{
	return container(r, out, 5, 16);
}

bool entelea_cbor_array_open(entelea_cbor_reader_t *r, uint32_t *out)
{
	return container(r, out, 4, 64);
}

bool entelea_cbor_uint(entelea_cbor_reader_t *r, uint64_t *out)
{
	if (r == NULL || out == NULL) {
		return false;
	}
	entelea_cbor_reader_t next = *r;
	uint8_t major;
	uint64_t value;

	if (!head(&next, &major, &value) || major != 0) {
		return false;
	}
	*out = value;
	*r = next;
	return true;
}

bool entelea_cbor_int(entelea_cbor_reader_t *r, int64_t *out)
{
	if (r == NULL || out == NULL) {
		return false;
	}
	entelea_cbor_reader_t next = *r;
	uint8_t major;
	uint64_t value;

	if (!head(&next, &major, &value) || major > 1 || value > INT64_MAX) {
		return false;
	}
	*out = major == 0 ? (int64_t)value : -1 - (int64_t)value;
	*r = next;
	return true;
}

bool entelea_cbor_bool(entelea_cbor_reader_t *r, bool *out)
{
	if (r == NULL || out == NULL || r->p == NULL || r->rem == 0 ||
	    (*r->p != 0xf4 && *r->p != 0xf5)) {
		return false;
	}
	*out = *r->p == 0xf5;
	r->p++;
	r->rem--;
	return true;
}

bool entelea_cbor_tstr(entelea_cbor_reader_t *r, char *out, size_t cap)
{
	if (r == NULL || out == NULL) {
		return false;
	}
	entelea_cbor_reader_t next = *r;
	uint8_t major;
	uint64_t value;

	if (!head(&next, &major, &value) || major != 3 || value >= cap || value > next.rem) {
		return false;
	}
	memcpy(out, next.p, (size_t)value);
	out[value] = '\0';
	next.p += (size_t)value;
	next.rem -= (size_t)value;
	*r = next;
	return true;
}

bool entelea_cbor_bstr_ref(entelea_cbor_reader_t *r, const uint8_t **out, uint32_t *len)
{
	if (r == NULL || out == NULL || len == NULL) {
		return false;
	}
	entelea_cbor_reader_t next = *r;
	uint8_t major;
	uint64_t value;

	if (!head(&next, &major, &value) || major != 2 || value > UINT32_MAX || value > next.rem) {
		return false;
	}
	*out = next.p;
	*len = (uint32_t)value;
	next.p += (size_t)value;
	next.rem -= (size_t)value;
	*r = next;
	return true;
}

static bool skip_value(entelea_cbor_reader_t *r, int depth)
{
	if (depth < 0 || depth > 4 || r->p == NULL || r->rem == 0) {
		return false;
	}
	uint8_t first = *r->p;
	uint8_t major;
	uint64_t value;

	if (!head(r, &major, &value)) {
		return false;
	}
	switch (major) {
	case 0:
	case 1:
		return true;
	case 2:
	case 3:
		if (value > 4096 || value > r->rem) {
			return false;
		}
		r->p += (size_t)value;
		r->rem -= (size_t)value;
		return true;
	case 4:
	case 5:
		if (value > (major == 4 ? 64U : 16U)) {
			return false;
		}
		for (uint64_t i = 0; i < (major == 4 ? value : value * 2); i++) {
			if (!skip_value(r, depth + 1)) {
				return false;
			}
		}
		return true;
	case 7:
		return first == 0xf4 || first == 0xf5 || first == 0xf6;
	default:
		return false;
	}
}

bool entelea_cbor_skip(entelea_cbor_reader_t *r, int depth)
{
	if (r == NULL) {
		return false;
	}
	entelea_cbor_reader_t next = *r;

	if (!skip_value(&next, depth)) {
		return false;
	}
	*r = next;
	return true;
}

static size_t head_size(uint64_t value)
{
	return value < 24 ? 1 : value <= UINT8_MAX ? 2 : value <= UINT16_MAX ? 3 :
	       value <= UINT32_MAX ? 5 : 9;
}

static void write_head(uint8_t *buf, size_t *pos, uint8_t major, uint64_t value)
{
	size_t bytes = head_size(value) - 1;
	uint8_t info = bytes == 0 ? (uint8_t)value : bytes == 1 ? 24 :
		       bytes == 2 ? 25 : bytes == 4 ? 26 : 27;

	buf[(*pos)++] = (uint8_t)((major << 5) | info);
	for (size_t i = bytes; i != 0; i--) {
		buf[(*pos)++] = (uint8_t)(value >> ((i - 1) * 8));
	}
}

static bool put_value(uint8_t *buf, size_t cap, size_t *pos, uint8_t major,
	uint64_t value, const void *payload, size_t len)
{
	if (buf == NULL || pos == NULL || *pos > cap || (payload == NULL && len != 0)) {
		return false;
	}
	size_t size = head_size(value);

	if (size > cap - *pos || len > cap - *pos - size) {
		return false;
	}
	write_head(buf, pos, major, value);
	if (len != 0) {
		memcpy(buf + *pos, payload, len);
		*pos += len;
	}
	return true;
}

bool entelea_cbor_put_map(uint8_t *buf, size_t cap, size_t *pos, uint32_t pairs)
{
	return put_value(buf, cap, pos, 5, pairs, NULL, 0);
}

bool entelea_cbor_put_array(uint8_t *buf, size_t cap, size_t *pos, uint32_t items)
{
	return put_value(buf, cap, pos, 4, items, NULL, 0);
}

bool entelea_cbor_put_uint(uint8_t *buf, size_t cap, size_t *pos, uint64_t value)
{
	return put_value(buf, cap, pos, 0, value, NULL, 0);
}

bool entelea_cbor_put_int(uint8_t *buf, size_t cap, size_t *pos, int64_t value)
{
	return put_value(buf, cap, pos, value >= 0 ? 0 : 1,
			 value >= 0 ? (uint64_t)value : (uint64_t)(-1 - value), NULL, 0);
}

bool entelea_cbor_put_bool(uint8_t *buf, size_t cap, size_t *pos, bool value)
{
	return put_value(buf, cap, pos, 7, value ? 21 : 20, NULL, 0);
}

bool entelea_cbor_put_tstrn(uint8_t *buf, size_t cap, size_t *pos, const char *s, size_t n)
{
	return put_value(buf, cap, pos, 3, n, s, n);
}

bool entelea_cbor_put_tstr(uint8_t *buf, size_t cap, size_t *pos, const char *s)
{
	return s != NULL && entelea_cbor_put_tstrn(buf, cap, pos, s, strlen(s));
}

bool entelea_cbor_put_bstr(uint8_t *buf, size_t cap, size_t *pos, const uint8_t *s, uint32_t n)
{
	return put_value(buf, cap, pos, 2, n, s, n);
}
