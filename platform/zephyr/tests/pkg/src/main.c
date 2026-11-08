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

/* 构包签名键可覆盖（默认=测试根=ENTEleA_TEST 工厂根；信任表判别性
 * 测试用非工厂键签名——计划符合性审查 P2：旧用例把测试根写入表，
 * 而工厂根本就在信任集内，表键分支无判别力）。 */
static const unsigned char *sign_sk = test_root_sk;

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

	memcpy(sm + tail, sign_sk + 32, 64); /* sig 占位（sign 前缀=sk[32:96]） */
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

	crypto_sign(signed_buf, &smlen, sm + 64 + (0), tail - 64, sign_sk);
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

/* ---- 信任表键独立生效（判别性）：仅存在于表的非工厂键 ---- */

/* tweetnacl keypair 需 randombytes；生产树刻意无 RNG（确定性纪律）——
 * 测试内确定性实现（固定种子序列）：只为生成一把与工厂根**无关**的
 * 密钥，无需密码学随机。 */
void randombytes(unsigned char *buf, unsigned long long len)
{
	unsigned char st = 0xA7;

	for (unsigned long long i = 0; i < len; i++) {
		st = (unsigned char)(st * 31 + i + 0x5A);
		buf[i] = st;
	}
}

ZTEST(framework_pkg, test_trust_table_only_key)
{
	entelea_trust_test_reset();
	struct pkgb p;

	/* 生成非工厂键（与测试根/工厂根无关的独立密钥对）。 */
	unsigned char pk2[32], sk2[64];

	zassert_equal(crypto_sign_keypair(pk2, sk2), 0);

	/* 前置差分：pk2 尚不在信任集（工厂根∪空表）→ 其签名包必须拒。 */
	sign_sk = sk2;
	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	stage_pkg(0, &p);
	entelea_tsap_view_t v;

	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL),
		      ENTELEA_E_INVALID_SIG, "非工厂键不在表=不受信");

	/* 信任表（次版）只写 pk2 → 同一包经 NULL 路径过——证明表键独立
	 * 生效（不是靠工厂根兜底）。 */
	static uint8_t trust_cbor[128];
	uint32_t n = 0;

	trust_cbor[n++] = 0xa2;
	memcpy(trust_cbor + n, "\x63ver", 4);
	n += 4;
	trust_cbor[n++] = 1;
	memcpy(trust_cbor + n, "\x64keys", 5);
	n += 5;
	trust_cbor[n++] = 0x81;
	trust_cbor[n++] = 0x58;
	trust_cbor[n++] = 32;
	memcpy(trust_cbor + n, pk2, 32);
	n += 32;
	zassert_equal(entelea_trust_write(trust_cbor, n), ENTELEA_OK);
	zassert_equal(entelea_pkg_verify(0, NULL, &v, NULL), ENTELEA_OK,
		      "仅信任表键验签过");

	/* 并集语义：表只含 pk2 时，工厂根签名包仍过（工厂根∪表）。 */
	sign_sk = test_root_sk;
	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	stage_pkg(1, &p);
	zassert_equal(entelea_pkg_verify(1, NULL, &v, NULL), ENTELEA_OK,
		      "并集：工厂根不受表内容影响");
	entelea_trust_test_reset();
	sign_sk = test_root_sk;
}

ZTEST(framework_pkg, test_stage_chain_and_activate)
{
	struct pkgb p;

	build_pkg(&p, MAN, sizeof(MAN), WASM, sizeof(WASM), 0);
	/* 越界 + 乱序 chunk 拒（顺序游标）。 */
	zassert_equal(entelea_pkg_stage_begin(0), ENTELEA_OK);
	zassert_equal(entelea_pkg_stage_chunk(0, ENTELEA_APP_SLOT_SIZE - 2, p.b, 4),
		      ENTELEA_E_RANGE);
	zassert_equal(entelea_pkg_stage_chunk(0, 4, p.b, 4), ENTELEA_E_RANGE,
		      "乱序拒（游标=0）");
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

/* ---- D03 Lua 可行性验证（子闭环 2；无解释器依赖——机制验证） ---- */
#include <entelea/lua_contract.h>

ZTEST(framework_pkg, test_lua_payload_ident)
{
	static const uint8_t lua53[] = {0x1b, 'L', 'u', 'a', 0x53};
	static const uint8_t lua54[] = {0x1b, 'L', 'u', 'a', 0x54};
	static const uint8_t wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00};
	entelea_lua_payload_info_t info;

	zassert_equal(entelea_lua_payload_ident(lua53, sizeof(lua53), &info),
		      ENTELEA_OK);
	zassert_equal(info.format, ENTELEA_LUA_FMT_BINARY);
	zassert_equal(info.version, 0x53);
	zassert_equal(entelea_lua_payload_ident(lua54, sizeof(lua54), &info),
		      ENTELEA_OK);
	zassert_equal(info.version, 0x54);
	/* 非 Lua 载荷（含 Wasm magic）fail-closed——不得按后缀/默认解释。 */
	zassert_equal(entelea_lua_payload_ident(wasm, sizeof(wasm), &info),
		      ENTELEA_E_INVALID_SIG);
	zassert_equal(entelea_lua_payload_ident(lua53, 3, &info),
		      ENTELEA_E_PARAM, "截断拒");
	zassert_equal(entelea_lua_payload_ident(NULL, 8, &info), ENTELEA_E_PARAM);
}

ZTEST(framework_pkg, test_lua_mem_budget_gate)
{
	entelea_lua_mem_budget_t a, b;

	zassert_equal(entelea_lua_mem_init(&a, 1000), ENTELEA_OK);
	zassert_equal(entelea_lua_mem_init(&b, 1000), ENTELEA_OK);
	zassert_true(entelea_lua_mem_alloc(&a, 0, 600));
	zassert_true(entelea_lua_mem_alloc(&a, 600, 800)); /* 扩 200 */
	zassert_equal(a.used_bytes, 800);
	zassert_false(entelea_lua_mem_alloc(&a, 800, 1001), "超配额拒");
	zassert_equal(a.denied, 1);
	zassert_true(entelea_lua_mem_alloc(&a, 800, 1000), "恰满通过");
	zassert_false(entelea_lua_mem_alloc(&a, 1000, 1001));
	zassert_true(entelea_lua_mem_alloc(&a, 1000, 0)); /* 释放 */
	zassert_equal(a.used_bytes, 0);
	zassert_equal(a.peak_bytes, 1000);
	/* 实例隔离：a 释放不影响 b 配额（无共享池）。 */
	zassert_false(entelea_lua_mem_alloc(&b, 0, 1001), "b 独立配额");
	zassert_equal(entelea_lua_mem_init(&a, 0), ENTELEA_E_PARAM);
}

ZTEST(framework_pkg, test_lua_exec_budget_bounded_stop)
{
	entelea_lua_exec_budget_t e;

	zassert_equal(entelea_lua_exec_init(&e, 10), ENTELEA_OK);
	for (int i = 0; i < 10; i++) {
		zassert_true(entelea_lua_exec_step(&e), "预算内步");
	}
	zassert_false(entelea_lua_exec_step(&e), "超预算停");
	zassert_true(e.aborted, "停止态置位");
	zassert_false(entelea_lua_exec_step(&e), "停止后一切步拒（有界）");
	zassert_equal(entelea_lua_exec_init(&e, 0), ENTELEA_E_PARAM);
}

ZTEST(framework_pkg, test_lua_contract_encode)
{
	const entelea_lua_contract_t good = {
		.version = 1, .mem_quota_kb = 64, .exec_budget_steps = 100000,
		.format = ENTELEA_LUA_FMT_BINARY,
	};
	uint8_t buf[ENTELEA_LUA_CONTRACT_CBOR_MAX];
	uint32_t n = 0;

	zassert_equal(entelea_lua_contract_encode(&good, buf, &n), ENTELEA_OK);
	zassert_true(n > 0 && n <= sizeof(buf));
	zassert_equal(buf[0], 0xa4, "map(4) 定序");
	/* 域外负测。 */
	entelea_lua_contract_t bad = good;

	bad.version = 2;
	zassert_equal(entelea_lua_contract_encode(&bad, buf, &n), ENTELEA_E_PARAM);
	bad = good;
	bad.mem_quota_kb = 513;
	zassert_equal(entelea_lua_contract_encode(&bad, buf, &n), ENTELEA_E_PARAM);
	bad = good;
	bad.mem_quota_kb = 0;
	zassert_equal(entelea_lua_contract_encode(&bad, buf, &n), ENTELEA_E_PARAM);
	bad = good;
	bad.exec_budget_steps = 0;
	zassert_equal(entelea_lua_contract_encode(&bad, buf, &n), ENTELEA_E_PARAM);
}
