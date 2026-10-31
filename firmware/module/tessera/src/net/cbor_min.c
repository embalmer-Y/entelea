/* SPDX-License-Identifier: Apache-2.0 */
/* 最小确定性 CBOR 编解码（LLD-ts-net §4 命令面 / §5 发布 payload）。
 * 解码 = definite map/array/tstr/uint/negint/bool 的严格子集（对齐 prov.c
 * 纪律：不定长/未知 major = 失败，fail-closed）；编码 = canonical 追加写。 */
#include <string.h>
#include "internal.h"

/* ---- 解码 --------------------------------------------------------------- */

void ts_cbor_rd_init(ts_cbor_rd_t *r, const uint8_t *buf, size_t len)
{
	r->p = buf;
	r->rem = len;
}

static bool rd_u8(ts_cbor_rd_t *r, uint8_t *out)
{
	if (r->rem < 1) {
		return false;
	}
	*out = *r->p++;
	r->rem--;
	return true;
}

static bool rd_head(ts_cbor_rd_t *r, uint8_t *maj, uint64_t *val)
{
	uint8_t ib;

	if (!rd_u8(r, &ib)) {
		return false;
	}
	*maj = (uint8_t)(ib >> 5);
	uint8_t info = (uint8_t)(ib & 0x1F);

	if (info < 24) {
		*val = info;
		return true;
	}
	int bytes = (info == 24) ? 1 : (info == 25) ? 2 : (info == 26) ? 4 : (info == 27) ? 8 : -1;

	if (bytes < 0 || r->rem < (size_t)bytes) {
		return false; /* 不定长(31)/保留 = 拒绝 */
	}
	uint64_t v = 0;

	for (int i = 0; i < bytes; i++) {
		v = (v << 8) | *r->p++;
	}
	r->rem -= (size_t)bytes;
	*val = v;
	return true;
}

bool ts_cbor_map_open(ts_cbor_rd_t *r, uint32_t *pairs)
{
	uint8_t maj;
	uint64_t v;

	if (!rd_head(r, &maj, &v) || maj != 5 || v > 16) {
		return false;
	}
	*pairs = (uint32_t)v;
	return true;
}

/* 通用值跳过（CG4 · Q-39②：scan 类走查跳过非目标键——不复制内容）。
 * definite-length 专用（不定长/浮点/断点 = false，与解码器族同纪律）；
 * 递归深度硬上界 4（manifest 嵌套面 needs 一层，余量防御）。 */
bool ts_cbor_skip(ts_cbor_rd_t *r, int depth)
{
	uint8_t maj;
	uint64_t v;

	if (depth > 4 || !rd_head(r, &maj, &v)) {
		return false;
	}
	switch (maj) {
	case 0: /* uint */
	case 1: /* nint */
		return true;
	case 2: /* bstr */
	case 3: /* tstr */
		if (v > 4096 || r->rem < (size_t)v) {
			return false;
		}
		r->p += (size_t)v;
		r->rem -= (size_t)v;
		return true;
	case 4: /* array */
		if (v > 64) {
			return false;
		}
		for (uint64_t i = 0; i < v; i++) {
			if (!ts_cbor_skip(r, depth + 1)) {
				return false;
			}
		}
		return true;
	case 5: /* map */
		if (v > 16) {
			return false;
		}
		for (uint64_t i = 0; i < 2 * v; i++) { /* 键 + 值 */
			if (!ts_cbor_skip(r, depth + 1)) {
				return false;
			}
		}
		return true;
	case 7: /* simple/bool/null/float——头部形态完备即可跳（rd_head 已消费定长载荷） */
		return true;
	default:
		return false; /* 6 tag：manifest 域不出现，fail-closed */
	}
}

bool ts_cbor_array_open(ts_cbor_rd_t *r, uint32_t *items)
{
	uint8_t maj;
	uint64_t v;

	if (!rd_head(r, &maj, &v) || maj != 4 || v > 64) {
		return false;
	}
	*items = (uint32_t)v;
	return true;
}

/* bstr 解码（MA3.1：app-chunk 数据载荷）。引用零拷贝版：不复制，回传
 * 指向请求缓冲内部的指针（生存期 = 分发调用域；fail-closed 同族）。 */
bool ts_cbor_bstr_ref(ts_cbor_rd_t *r, const uint8_t **out, uint32_t *len)
{
	uint8_t maj;
	uint64_t v;

	if (!rd_head(r, &maj, &v) || maj != 2 || v > UINT32_MAX || v > r->rem) {
		return false;
	}
	*out = r->p;
	*len = (uint32_t)v;
	r->p += v;
	r->rem -= (size_t)v;
	return true;
}

bool ts_cbor_tstr(ts_cbor_rd_t *r, char *out, size_t cap)
{
	uint8_t maj;
	uint64_t v;

	if (!rd_head(r, &maj, &v) || maj != 3 || v >= cap || v > r->rem) {
		return false;
	}
	memcpy(out, r->p, (size_t)v);
	out[v] = '\0';
	r->p += v;
	r->rem -= (size_t)v;
	return true;
}

bool ts_cbor_uint(ts_cbor_rd_t *r, uint64_t *out)
{
	uint8_t maj;
	uint64_t v;

	if (!rd_head(r, &maj, &v) || maj != 0) {
		return false;
	}
	*out = v;
	return true;
}

bool ts_cbor_int(ts_cbor_rd_t *r, int64_t *out)
{
	uint8_t maj;
	uint64_t v;

	if (!rd_head(r, &maj, &v)) {
		return false;
	}
	if (maj == 0) {
		*out = (int64_t)v;
		return true;
	}
	if (maj == 1 && v <= (uint64_t)INT64_MAX) {
		*out = -1 - (int64_t)v;
		return true;
	}
	return false;
}

bool ts_cbor_bool(ts_cbor_rd_t *r, bool *out)
{
	uint8_t maj;
	uint64_t v;

	if (!rd_head(r, &maj, &v) || maj != 7) {
		return false;
	}
	/* RFC 8949：true=21/false=20（写侧 K4 修正同源）；simple 1/0 为历史
	 * 自家方言——读侧一并规范化拒收（fail-closed，未知 simple 值即拒）。 */
	if (v == 21) {
		*out = true;
		return true;
	}
	if (v == 20) {
		*out = false;
		return true;
	}
	return false;
}

/* ---- 编码 --------------------------------------------------------------- */

static bool put_head(uint8_t *buf, size_t cap, size_t *pos, uint8_t maj, uint64_t v)
{
	if (v < 24) {
		if (*pos + 1 > cap) return false;
		buf[(*pos)++] = (uint8_t)((maj << 5) | (uint8_t)v);
		return true;
	}
	int bytes = (v <= 0xFF) ? 1 : (v <= 0xFFFF) ? 2 : (v <= 0xFFFFFFFFU) ? 4 : 8;
	/* additional-info：1B=24 / 2B=25 / 4B=26 / 8B=27（0x1F=31 是不定长，禁用） */
	uint8_t info = (bytes == 1) ? 24 : (bytes == 2) ? 25 : (bytes == 4) ? 26 : 27;

	if (*pos + 1 + (size_t)bytes > cap) return false;
	buf[(*pos)++] = (uint8_t)((maj << 5) | info);
	for (int i = bytes - 1; i >= 0; i--) {
		buf[(*pos)++] = (uint8_t)(v >> (8 * i));
	}
	return true;
}

bool ts_cbor_put_map(uint8_t *buf, size_t cap, size_t *pos, uint32_t pairs)
{
	return put_head(buf, cap, pos, 5, pairs);
}

bool ts_cbor_put_array(uint8_t *buf, size_t cap, size_t *pos, uint32_t items)
{
	return put_head(buf, cap, pos, 4, items);
}

bool ts_cbor_put_tstrn(uint8_t *buf, size_t cap, size_t *pos, const char *s, size_t n)
{
	if (!put_head(buf, cap, pos, 3, n)) return false;
	if (*pos + n > cap) return false;
	memcpy(buf + *pos, s, n);
	*pos += n;
	return true;
}

bool ts_cbor_put_tstr(uint8_t *buf, size_t cap, size_t *pos, const char *s)
{
	return ts_cbor_put_tstrn(buf, cap, pos, s, strlen(s));
}

bool ts_cbor_put_bstr(uint8_t *buf, size_t cap, size_t *pos, const uint8_t *b, uint32_t len)
{
	if (!put_head(buf, cap, pos, 2, len)) return false;
	if (*pos + len > cap) return false;
	memcpy(buf + *pos, b, len);
	*pos += len;
	return true;
}

bool ts_cbor_put_uint(uint8_t *buf, size_t cap, size_t *pos, uint64_t v)
{
	return put_head(buf, cap, pos, 0, v);
}

bool ts_cbor_put_int(uint8_t *buf, size_t cap, size_t *pos, int64_t v)
{
	if (v >= 0) {
		return put_head(buf, cap, pos, 0, (uint64_t)v);
	}
	return put_head(buf, cap, pos, 1, (uint64_t)(-1 - v));
}

bool ts_cbor_put_bool(uint8_t *buf, size_t cap, size_t *pos, bool v)
{
	if (*pos + 1 > cap) return false;
	/* RFC 8949：true = 0xF5 / false = 0xF4（major 7 simple 21/20）。
	 * K4 真机定案：初版 (7<<5)|1 = 0xE1 = simple value 1 ≠ CBOR true——
	 * 严格解码方（agent cbor2）得 CBORSimpleValue(1)，`is not True` 即拒；
	 * FakeCube/sim 同错镜像从未暴露（F5 同族：编码面须严格消费方实检）。 */
	buf[(*pos)++] = v ? 0xF5 : 0xF4;
	return true;
}
