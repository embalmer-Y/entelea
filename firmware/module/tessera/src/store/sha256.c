/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SHA-256（FIPS 180-4）自包含实现——ts_store_slot_hash 供安装校验（LLD-ts-store §6）。
 * 自包含理由：M2a 仅需摘要；M2b 验签密码学由 WAMR/ed25519 侧引入，届时统一评估。
 * 向量测试见 firmware/tests/store（"abc" 标准向量）。
 */
#include <string.h>
#include "internal.h"

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
	0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
	0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
	0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
	0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
	0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t p[64])
{
	uint32_t w[64];

	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
	uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = hh + S1 + ch + K[i] + w[i];
		uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + mj;

		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

void ts_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	ts_sha256_ctx_t ctx;

	ts_sha256_init(&ctx);
	ts_sha256_update(&ctx, data, (uint32_t)len);
	ts_sha256_final(&ctx, out);
}

void ts_sha256_init(ts_sha256_ctx_t *ctx)
{
	ctx->h[0] = 0x6a09e667;
	ctx->h[1] = 0xbb67ae85;
	ctx->h[2] = 0x3c6ef372;
	ctx->h[3] = 0xa54ff53a;
	ctx->h[4] = 0x510e527f;
	ctx->h[5] = 0x9b05688c;
	ctx->h[6] = 0x1f83d9ab;
	ctx->h[7] = 0x5be0cd19;
	ctx->len = 0;
	ctx->buf_len = 0;
}

void ts_sha256_update(ts_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
	ctx->len += len;
	while (len > 0) {
		uint32_t n = 64 - ctx->buf_len;

		if (n > len) {
			n = len;
		}
		memcpy(ctx->buf + ctx->buf_len, data, n);
		ctx->buf_len += n;
		data += n;
		len -= n;
		if (ctx->buf_len == 64) {
			sha256_block(ctx->h, ctx->buf);
			ctx->buf_len = 0;
		}
	}
}

void ts_sha256_final(ts_sha256_ctx_t *ctx, uint8_t out[32])
{
	uint64_t bits = ctx->len << 3;

	ctx->buf[ctx->buf_len++] = 0x80;
	if (ctx->buf_len > 56) {
		memset(ctx->buf + ctx->buf_len, 0, 64 - ctx->buf_len);
		sha256_block(ctx->h, ctx->buf);
		ctx->buf_len = 0;
	}
	memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);
	for (int i = 0; i < 8; i++) {
		ctx->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha256_block(ctx->h, ctx->buf);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(ctx->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)ctx->h[i];
	}
}
