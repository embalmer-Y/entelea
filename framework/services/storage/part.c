/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 分区命名空间与后端端口（迁自旧 store/part.c 的 RAM 后端子集，R03 合同
 * framework/services/storage/contract.md）。
 * RAM 后端（native_sim/主机测试）：静态数组 + 0xFF 抹除语义，
 * test_reset 模拟"掉电后全新镜像"。flash 后端（DT fixed-partitions +
 * flash_map，含写对齐垫片）随板级批（D12 冻结后）迁移——本子闭环不提供，
 * 不宣称板级持久化已验证。越界（含 off+len u32 回绕）显式拒绝。
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define META_SIZE 1024U   /* 双副本各 512B */
#define NOINIT_SIZE 256U
#define AUDIT_SIZE 8192U  /* 两页乒乓 × 64 槽 × 64B（DEC-52①） */
#define PROV_SIZE 4096U
#define TRUST_SIZE 8192U
#define KV_SIZE 8192U
#define SLOT_SIZE 262144U

static uint8_t ram_meta[META_SIZE];
static uint8_t ram_noinit[NOINIT_SIZE];
static uint8_t ram_audit[AUDIT_SIZE];
static uint8_t ram_prov[PROV_SIZE];
static uint8_t ram_trust[TRUST_SIZE];
static uint8_t ram_kv[KV_SIZE];
static uint8_t ram_slot_a[SLOT_SIZE];
static uint8_t ram_slot_b[SLOT_SIZE];

static uint8_t *part_ram(entelea_store_part_t part, uint32_t *size)
{
	switch (part) {
	case ENTELEA_PART_META:
		*size = META_SIZE;
		return ram_meta;
	case ENTELEA_PART_NOINIT:
		*size = NOINIT_SIZE;
		return ram_noinit;
	case ENTELEA_PART_AUDIT:
		*size = AUDIT_SIZE;
		return ram_audit;
	case ENTELEA_PART_PROV:
		*size = PROV_SIZE;
		return ram_prov;
	case ENTELEA_PART_TRUST:
		*size = TRUST_SIZE;
		return ram_trust;
	case ENTELEA_PART_KV:
		*size = KV_SIZE;
		return ram_kv;
	case ENTELEA_PART_SLOT_A:
		*size = SLOT_SIZE;
		return ram_slot_a;
	case ENTELEA_PART_SLOT_B:
		*size = SLOT_SIZE;
		return ram_slot_b;
	default:
		*size = 0;
		return NULL;
	}
}

static entelea_result_t ram_read(entelea_store_part_t part, uint32_t off, void *buf,
				 uint32_t len)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	memcpy(buf, base + off, len);
	return ENTELEA_OK;
}

static entelea_result_t ram_write(entelea_store_part_t part, uint32_t off,
				  const void *buf, uint32_t len)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	memcpy(base + off, buf, len);
	return ENTELEA_OK;
}

static entelea_result_t ram_erase_off(entelea_store_part_t part, uint32_t off,
				      uint32_t len)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	memset(base + off, 0xFF, len);
	return ENTELEA_OK;
}

static entelea_result_t ram_erase(entelea_store_part_t part)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL) {
		return ENTELEA_E_PARAM;
	}
	memset(base, 0xFF, size);
	return ENTELEA_OK;
}

static uint32_t ram_size(entelea_store_part_t part)
{
	uint32_t size;

	return part_ram(part, &size) == NULL ? 0 : size;
}

const entelea_store_ops_t entelea_store_backend = {
	.read = ram_read,
	.write = ram_write,
	.erase = ram_erase,
	.erase_off = ram_erase_off,
	.size = ram_size,
};

#ifdef ENTELEA_TEST
/* 越界/回绕负测探针：直通后端 read（端口边界合同）。 */
entelea_result_t entelea_storage_test_backend_read(entelea_store_part_t part,
						   uint32_t off, void *buf,
						   uint32_t len)
{
	return entelea_store_backend.read(part, off, buf, len);
}

entelea_result_t entelea_storage_test_backend_write(entelea_store_part_t part,
						    uint32_t off, const void *buf,
						    uint32_t len)
{
	return entelea_store_backend.write(part, off, buf, len);
}

entelea_result_t entelea_storage_test_reset(void)
{
	for (int p = 0; p < ENTELEA_PART_COUNT; p++) {
		if (entelea_store_backend.size((entelea_store_part_t)p) == 0) {
			continue;
		}
		entelea_result_t r = entelea_store_backend.erase((entelea_store_part_t)p);

		if (r != ENTELEA_OK) {
			return r;
		}
	}
	return ENTELEA_OK;
}
#endif

/* 公共端口原语（后端表保持模块私有；跨模块消费经此窄面）。 */
entelea_result_t entelea_storage_read(entelea_store_part_t part, uint32_t off,
				      void *buf, uint32_t len)
{
	return ram_read(part, off, buf, len);
}

entelea_result_t entelea_storage_write(entelea_store_part_t part, uint32_t off,
				       const void *buf, uint32_t len)
{
	return ram_write(part, off, buf, len);
}

entelea_result_t entelea_storage_erase(entelea_store_part_t part)
{
	return ram_erase(part);
}

uint32_t entelea_storage_part_size(entelea_store_part_t part)
{
	return ram_size(part);
}

/* ---- SHA-256 流式（R05 包验签；FIPS 180-4，与 R01 一次性接口同算法）----
 * 调用方串行（包验签单线程域）；state 由调用方持有（init→update*→final）。 */
#include <entelea/sha256.h>

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t SHA_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
	0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
	0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
	0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
	0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
	0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(uint32_t state[8], const uint8_t p[64])
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
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + SHA_K[i] + w[i];
		uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + mj;

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

struct s256ctx {
	uint32_t state[8];
	uint8_t buf[64];
	uint32_t buflen;
	uint64_t total;
};
static struct s256ctx sc;

void entelea_sha256_stream_init(uint32_t state[8])
{
	static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
				       0xa54ff53a, 0x510e527f, 0x9b05688c,
				       0x1f83d9ab, 0x5be0cd19};

	memcpy(state, H0, 32);
	memset(&sc, 0, sizeof(sc));
	memcpy(sc.state, state, 32);
}

static void sc_absorb(const uint8_t *data, uint32_t len)
{
	sc.total += len;
	while (len > 0) {
		uint32_t n = 64 - sc.buflen;

		if (n > len) {
			n = len;
		}
		memcpy(sc.buf + sc.buflen, data, n);
		sc.buflen += n;
		data += n;
		len -= n;
		if (sc.buflen == 64) {
			sha256_block(sc.state, sc.buf);
			sc.buflen = 0;
		}
	}
}

void entelea_sha256_stream_update(uint32_t state[8], const uint8_t *data,
				  uint32_t len)
{
	if (data == NULL && len > 0) {
		return;
	}
	/* state 回填（调用方持有可能被复制的状态；幂等）。 */
	memcpy(sc.state, state, 32);
	sc_absorb(data, len);
	memcpy(state, sc.state, 32);
}

void entelea_sha256_stream_final(const uint32_t state[8], uint8_t out[32])
{
	memcpy(sc.state, state, 32);
	uint64_t bits = sc.total * 8;
	uint8_t pad = 0x80;

	sc_absorb(&pad, 1);
	uint8_t z = 0;

	while (sc.buflen != 56) {
		sc_absorb(&z, 1);
	}
	uint8_t lenb[8];

	for (int i = 0; i < 8; i++) {
		lenb[i] = (uint8_t)(bits >> (56 - i * 8));
	}
	sc_absorb(lenb, 8);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(sc.state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(sc.state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(sc.state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)sc.state[i];
	}
	memset(&sc, 0, sizeof(sc));
}
