/* SPDX-License-Identifier: Apache-2.0 */
/* R05 包事实测试：TSAP v2 头 oracle / 验签全管线（好签名/坏签名/签A装B/
 * 位腐/信任集）/ 暂存-激活链。签名在测试内用 tweetnacl crypto_sign 以
 * 固定测试根私钥（旧 Forge fixtures 公开材料）构造——同一双实现纪律。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/appstore.h>
#include <entelea/package.h>
#include <entelea/storage.h>
#include <entelea/trust.h>
#include "crypto/tweetnacl.h"

/* 旧测试根私钥（agent/tests/fixtures/ts-test-root.key——64B sk；
 * 公开测试材料，Q-29④：测试信任≠生产信任）。 */
static const unsigned char test_root_sk[64] = {
#include "test_root_sk.inc"
};

/* ---- 包构造器（canonical COSE_Sign1） ---- */
#define PKG_MAX 4096
struct pkgb {
	uint8_t b[PKG_MAX];
	uint32_t n;
};

static void pb8(struct pkgb *p, uint8_t v) { p->b[p->n++] = v; }

static void pb_be16(struct pkgb *p, uint16_t v)
{
	pb8(p, (uint8_t)(v >> 8));
	pb8(p, (uint8_t)v);
}

static void pb_be32(struct pkgb *p, uint32_t v)
{
	pb8(p, (uint8_t)(v >> 24));
	pb8(p, (uint8_t)(v >> 16));
	pb8(p, (uint8_t)(v >> 8));
	pb8(p, (uint8_t)v);
}

static void pb_bstr_hdr(struct pkgb *p, uint32_t len)
{
	if (len < 24) {
		pb8(p, (uint8_t)(0x40 | len));
	} else if (len < 256) {
		pb8(p, 0x58);
		pb8(p, (uint8_t)len);
	} else if (len < 65536) {
		pb8(p, 0x59);
		pb8(p, (uint8_t)(len >> 8));
		pb8(p, (uint8_t)len);
	} else {
		pb8(p, 0x5A);
		pb_be32(p, len);
	}
}

static void build_pkg(struct pkgb *p, const uint8_t *manifest, uint32_t mlen,
		      const uint8_t *wasm, uint32_t wlen, int corrupt)
{
	memset(p, 0, sizeof(*p));
	p->b[0] = 'T'; p->b[1] = 'S'; p->b[2] = 'A'; p->b[3] = 'P';
	p->n = 4;
	pb_be16(p, 2); /* fmt_ver */
	pb_be32(p, mlen);
	pb_be32(p, wlen);
	pb_be16(p, 1); /* flags = digest */
	p->n = ENTELEA_TSAP_DIGEST_OFF;

	/* digest = sha256(manifest ‖ wasm) —— 经流式接口。 */
	uint32_t st[8];
	uint8_t digest[32];

	entelea_sha256_stream_init(st);
	entelea_sha256_stream_update(st, manifest, mlen);
	entelea_sha256_stream_update(st, wasm, wlen);
	entelea_sha256_stream_final(st, digest);
	memcpy(p->b + p->n, digest, 32);
	p->n += 32;
	memcpy(p->b + p->n, manifest, mlen);
	p->n += mlen;
	memcpy(p->b + p->n, wasm, wlen);
	p->n += wlen;

	/* COSE_Sign1: tag18 + array4 [phdr, uhdr{}, payload bstr, sig bstr(64)] */
	uint8_t payload[PKG_MAX / 2];
	uint32_t plen = mlen + wlen;

	memcpy(payload, manifest, mlen);
	memcpy(payload + mlen, wasm, wlen);

	uint8_t sm[64 + 22 + sizeof(payload)];
	size_t tail = 0;

	memcpy(sm + tail, test_root_sk + 32, 64); /* sig 占位（sign 前缀=sk[32:96]） */
	tail = 64;
	sm[tail++] = 0x84;
	sm[tail++] = 0x6a;
	memcpy(sm + tail, "Signature1", 10);
	tail += 10;
	sm[tail++] = 0x43;
	sm[tail++] = 0xa1;
	sm[tail++] = 0x01;
	sm[tail++] = 0x27;
	sm[tail++] = 0x40;
	if (plen < 24) {
		sm[tail++] = (uint8_t)(0x40 | plen);
	} else if (plen < 256) {
		sm[tail++] = 0x58;
		sm[tail++] = (uint8_t)plen;
	} else if (plen < 65536) {
		sm[tail++] = 0x59;
		sm[tail++] = (uint8_t)(plen >> 8);
		sm[tail++] = (uint8_t)plen;
	} else {
		sm[tail++] = 0x5A;
		sm[tail++] = (uint8_t)(plen >> 24);
		sm[tail++] = (uint8_t)(plen >> 16);
		sm[tail++] = (uint8_t)(plen >> 8);
		sm[tail++] = (uint8_t)plen;
	}
	memcpy(sm + tail, payload, plen);
	tail += plen;

	unsigned long long smlen = 0;

	/* crypto_sign(sm_signed, &smlen, m, mlen, sk)：输出 = sig(64)‖m。 */
	static uint8_t signed_buf[64 + sizeof(payload)];

	crypto_sign(signed_buf, &smlen, sm + 64 + (0), tail - 64, test_root_sk);
	/* signed_buf = 64B sig ‖ Sig_structure-minus-sig（我们构造的
	 * Sig_structure 前缀+payload）——即 sm 完整形态。 */
	pb8(p, 0xd2); /* tag 18 */
	pb8(p, 0x84); /* array(4) */
	pb8(p, 0x43); /* bstr(3) phdr */
	pb8(p, 0xa1); pb8(p, 0x01); pb8(p, 0x27);
	pb8(p, 0xa0); /* map(0) uhdr */
	pb_bstr_hdr(p, plen);
	memcpy(p->b + p->n, payload, plen);
	p->n += plen;
	pb8(p, 0x58);
	pb8(p, 64);
	memcpy(p->b + p->n, signed_buf, 64); /* sig */
	p->n += 64;
	(void)smlen;
	if (corrupt == 1 && p->n > ENTELEA_TSAP_CONTENT_OFF) {
		p->b[ENTELEA_TSAP_CONTENT_OFF] ^= 0xFF; /* 位腐（摘要面） */
	}
}

static const uint8_t MAN[] = {0xa1, 0x63, 'a', 'p', 'p', 0x01}; /* {"app":1} */
static const uint8_t WASM[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

static void stage_pkg(uint8_t slot, const struct pkgb *p)
{
	zassert_equal(entelea_pkg_stage_begin(slot), ENTELEA_OK);
	zassert_equal(entelea_pkg_stage_chunk(slot, 0, p->b, p->n), ENTELEA_OK);
}

static void *pkg_setup(void)
{
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	return NULL;
}

ZTEST(framework_pkg, test_tsap_header_oracle)
{
	struct pkgb p;

	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	entelea_tsap_view_t v;

	zassert_true(entelea_tsap_parse(p.b, p.n, &v));
	zassert_equal(v.fmt_ver, 2);
	zassert_equal(v.manifest_len, sizeof(MAN));
	zassert_equal(v.wasm_len, sizeof(WASM));
	zassert_equal(v.manifest_off, ENTELEA_TSAP_CONTENT_OFF);
	zassert_equal(v.wasm_off, ENTELEA_TSAP_CONTENT_OFF + sizeof(MAN));
	zassert_equal(v.cose_off, v.wasm_off + sizeof(WASM));

	/* 负测：magic/v1/flags/零长/截断。 */
	uint8_t h[16];

	memcpy(h, p.b, 16);
	h[0] = 'X';
	zassert_false(entelea_tsap_parse(h, sizeof(h), &v), "magic");
	memcpy(h, p.b, 16);
	h[4] = 0;
	h[5] = 1; /* v1 */
	zassert_false(entelea_tsap_parse(h, sizeof(h), &v), "v1 fail-closed");
	memcpy(h, p.b, 16);
	h[14] = 0;
	h[15] = 2; /* 未知 flags */
	zassert_false(entelea_tsap_parse(h, sizeof(h), &v), "flags");
	memcpy(h, p.b, 16);
	h[6] = h[7] = h[8] = h[9] = 0; /* manifest_len=0 */
	zassert_false(entelea_tsap_parse(h, sizeof(h), &v), "零长段");
	zassert_false(entelea_tsap_parse(p.b, 10, &v), "截断");
}

ZTEST(framework_pkg, test_verify_good_and_bad)
{
	struct pkgb p;

	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	stage_pkg(0, &p);
	entelea_tsap_view_t v;
	uint8_t digest[32];

	zassert_equal(entelea_pkg_verify(0, NULL, &v, digest), ENTELEA_OK,
		      "好签名（有效信任集=测试根）");
	/* 同键显式路径。 */
	zassert_equal(entelea_pkg_verify(0, entelea_pkg_test_root_pub, &v, NULL),
		      ENTELEA_OK);

	/* 位腐（manifest 首字节翻转 → 摘要不符）。 */
	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 1);
	stage_pkg(1, &p);
	zassert_equal(entelea_pkg_verify(1, NULL, &v, NULL),
		      ENTELEA_E_INVALID_SIG, "位腐拒");

	/* 签 A 装 B：好包 + 好签名，但裸 wasm 段被替换（内嵌副本≠裸段）。 */
	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	stage_pkg(0, &p);
	uint8_t rogue = 0xFF;

	zassert_equal(entelea_pkg_slot_write(0, ENTELEA_TSAP_CONTENT_OFF +
						     sizeof(MAN),
					     &rogue, 1),
		      ENTELEA_OK);
	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL),
		      ENTELEA_E_INVALID_SIG, "签A装B拒");

	/* 坏根（随机 32B）→ 验签失败。 */
	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	stage_pkg(1, &p);
	uint8_t wrong_root[32] = {1};

	zassert_equal(entelea_pkg_verify(1, wrong_root, &v, NULL),
		      ENTELEA_E_INVALID_SIG, "坏根拒");
	/* 信任集路径：信任表加入好根（次版）→ NULL 路径过。 */
	static uint8_t trust_cbor[128];
	uint32_t n = 0;

	trust_cbor[n++] = 0xa2;
	memcpy(trust_cbor + n, "\x63ver", 4);
	n += 4;
	trust_cbor[n++] = 1;
	memcpy(trust_cbor + n, "\x64keys", 5);
	n += 5;
	trust_cbor[n++] = 0x81; /* array(1) */
	trust_cbor[n++] = 0x58;
	trust_cbor[n++] = 32;
	memcpy(trust_cbor + n, entelea_pkg_test_root_pub, 32);
	n += 32;
	zassert_equal(entelea_trust_write(trust_cbor, n), ENTELEA_OK);
	zassert_equal(entelea_pkg_verify(1, NULL, &v, NULL), ENTELEA_OK,
		      "信任集路径（表键过）");
}

ZTEST(framework_pkg, test_stage_chain_and_activate)
{
	struct pkgb p;

	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	/* 越界 chunk 拒。 */
	zassert_equal(entelea_pkg_stage_begin(0), ENTELEA_OK);
	zassert_equal(entelea_pkg_stage_chunk(0, ENTELEA_APP_SLOT_SIZE - 2, p.b, 4),
		      ENTELEA_E_RANGE);
	zassert_equal(entelea_pkg_stage_chunk(0, 0, p.b, p.n), ENTELEA_OK);
	entelea_tsap_view_t v;

	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL), ENTELEA_OK);

	uint8_t slot = 9;

	zassert_equal(entelea_pkg_active(&slot), ENTELEA_E_UNINIT, "无 active");
	zassert_equal(entelea_pkg_activate(0), ENTELEA_OK);
	zassert_equal(entelea_pkg_active(&slot), ENTELEA_OK);
	zassert_equal(slot, 0);
	/* 未验签的 slot 也可"激活"（激活=事实记录；装载链在执行侧复核验签
	 * ——双层纪律，如实呈现）。 */
	zassert_equal(entelea_pkg_activate(1), ENTELEA_OK);
	zassert_equal(entelea_pkg_active(&slot), ENTELEA_OK);
	zassert_equal(slot, 1);
}

ZTEST_SUITE(framework_pkg, NULL, pkg_setup, NULL, NULL, NULL);
