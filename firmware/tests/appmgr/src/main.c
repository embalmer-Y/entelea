/* SPDX-License-Identifier: Apache-2.0 */
/* ts-appmgr L1/L2 测试（LLD-ts-appmgr §6）：包安装链 / slot 切换 /
 * 回滚与隔离 / meta 掉电恢复。
 * DEC-49（Q-29④）：真验签语义——夹具 = 预签 v2 包（测试根签名，
 * fixture_pkg.h 机械生成）；篡改矩阵在副本上翻字节断言 INVALID_SIG。
 * IR2-07：回滚目标槽验证（坏/空槽不翻转）+ meta 双损如实 E_IO。 */
#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>
#include <ts/appmgr.h>
#include <ts/store.h>
#include <ts/tsap.h>

#include "fixture_pkg.h"

static void *appmgr_setup(void)
{
	ts_store_test_reset();
	ts_appmgr_test_reset();
	return NULL;
}

#define PKG_BUFFER_SIZE 1024

/* fixture 头字段（生成期事实，运行期自头读——与容器一致性对拍） */
static uint32_t fx_ml, fx_wl, fx_total;

static void fx_fields(void)
{
	fx_ml = ((uint32_t)fixture_pkg[6] << 24) | ((uint32_t)fixture_pkg[7] << 16) |
		((uint32_t)fixture_pkg[8] << 8) | fixture_pkg[9];
	fx_wl = ((uint32_t)fixture_pkg[10] << 24) | ((uint32_t)fixture_pkg[11] << 16) |
		((uint32_t)fixture_pkg[12] << 8) | fixture_pkg[13];
	fx_total = sizeof(fixture_pkg);
}

static const uint8_t root_key[32] = {0}; /* TEST 构建：验签对象 = 固定测试根 */

ZTEST(framework_appmgr, test_install_and_slot_switch)
{
	/* ztest 执行序为链接布局（实现定义）——fresh meta 前置条件自足化 */
	ts_store_test_reset();
	fx_fields();
	ts_app_info_t info;

	ts_res_t ir = ts_appmgr_install(fixture_pkg, fx_total, root_key, &info);

	zassert_equal(ir, TS_OK, "预签 v2 包安装（真 ed25519 验签）");
	zassert_equal(info.state, TS_APP_STAGED);
	zassert_equal(info.active_slot, 1, "first install → slot B（active=0^1）");

	uint8_t rb[4];

	zassert_equal(ts_store_slot_read(1, 0, rb, 4), TS_OK);
	zassert_equal(rb[0], 'T', "slot B starts with TSAP magic");

	/* 二次安装 → slot A */
	ts_appmgr_test_reset();
	zassert_equal(ts_appmgr_install(fixture_pkg, fx_total, root_key, &info), TS_OK);
	zassert_equal(info.active_slot, 0, "second install → slot A（active=1^1=0）");
}

/* V0.4.0 M6（Q-32②）：多 entry 语义——独立乒乓对互不干扰 ---------------------- */

ZTEST(framework_appmgr, test_multi_entry_pingpong)
{
	/* entry e 槽对 = {2e, 2e+1}：entry0 用 {0,1}、entry1 用 {2,3}；
	 * meta count 扩展；跨 entry activate（begin/activate entry 不符）拒绝。
	 * M7 前运行时单实例——本用例验证存储面语义（安装/乒乓/持久/隔离）。 */
	ts_store_test_reset();
	ts_appmgr_test_reset();

	ts_app_info_t i0;
	zassert_equal(ts_appmgr_install(fixture_pkg, sizeof(fixture_pkg),
					root_key, &i0), TS_OK);
	zassert_equal(i0.active_slot, 1, "entry0 首装 → slot 1");

	uint8_t slot_out = 0xFF;
	zassert_equal(ts_appmgr_stage_begin_n(sizeof(fixture_pkg), 1, &slot_out),
		      TS_OK);
	zassert_equal(slot_out, 3, "entry1 首装 → slot 3（对基 2 ^1）");
	zassert_equal(ts_appmgr_stage_chunk(0, fixture_pkg, sizeof(fixture_pkg),
					    NULL), TS_OK);
	zassert_equal(ts_appmgr_stage_verify(root_key, NULL, NULL, NULL), TS_OK);
	zassert_equal(ts_appmgr_stage_activate_n(0, NULL), TS_E_STATE,
		      "entry 不符 activate 拒绝（fail-closed）");
	ts_app_info_t i1;
	zassert_equal(ts_appmgr_stage_activate_n(1, &i1), TS_OK);
	zassert_equal(i1.active_slot, 3, "entry1 激活 slot 3");

	ts_appmgr_meta_t meta;
	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.count, 2, "meta count 扩展");
	zassert_equal(meta.entries[0].active_slot, 1, "entry0 不受扰");
	zassert_equal(meta.entries[1].active_slot, 3, "entry1 持久");

	/* entry1 二次安装 → 对内乒乓 → slot 2 */
	ts_appmgr_test_reset();
	zassert_equal(ts_appmgr_stage_begin_n(sizeof(fixture_pkg), 1, &slot_out),
		      TS_OK);
	zassert_equal(slot_out, 2, "entry1 二装 → slot 2（3^1 对内翻转）");

	/* 越界 entry 拒绝 */
	zassert_equal(ts_appmgr_stage_begin_n(sizeof(fixture_pkg), 2, NULL),
		      TS_E_PARAM, "entry ≥ MAX_INSTANCES 拒绝");
}

ZTEST(framework_appmgr, test_install_tamper_matrix)
{
	fx_fields();
	static uint8_t pkg[PKG_BUFFER_SIZE];
	ts_app_info_t info;

	/* 坏 magic */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[0] = 'X';
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG);

	/* v1 头（fmt_ver=1）= fail-closed 拒收（DEC-49②：无兼容装载） */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[4] = 0; pkg[5] = 1; pkg[14] = 0; pkg[15] = 0;
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG, "v1 拒收");

	/* 未知 flags */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[15] = 0x02;
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG);

	/* 篡改摘要（位腐 → sha256 对拍失败） */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[16] ^= 0x01;
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG, "digest 篡改拒收");

	/* 篡改 manifest 内容字节（摘要与签名双双失败） */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[TSAP_CONTENT_OFF] ^= 0x01;
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG, "manifest 篡改拒收");

	/* 篡改 wasm 字节 */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[TSAP_CONTENT_OFF + fx_ml] ^= 0x01;
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG, "wasm 篡改拒收");

	/* 篡改签名（末 64B 翻一位 → ed25519 拒收） */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[fx_total - 1] ^= 0x01;
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG, "sig 篡改拒收");

	/* 非测试根签名的包 = 拒收：构造双字段错位（签名段首字节破坏） */
	memcpy(pkg, fixture_pkg, fx_total);
	pkg[fx_total - 70] ^= 0x01;
	zassert_equal(ts_appmgr_install(pkg, fx_total, root_key, &info),
		      TS_E_INVALID_SIG);

	/* 截断 */
	zassert_equal(ts_appmgr_install(fixture_pkg, 10, root_key, &info),
		      TS_E_INVALID_SIG);
}

ZTEST(framework_appmgr, test_rollback_closed_loop)
{
	/* 套件级 setup 不逐用例复位——本用例自取确定基线 */
	ts_store_test_reset();
	ts_appmgr_test_reset();

	/* IR2-07：单槽已装 → 回滚目标槽为空 → 如实失败不翻转 */
	ts_app_info_t info;

	zassert_equal(ts_appmgr_install(fixture_pkg, sizeof(fixture_pkg),
					root_key, &info), TS_OK);
	zassert_equal(info.active_slot, 1, "净基线首装 slot B（slot A 空）");
	extern ts_app_info_t current_apps[CONFIG_TS_APP_MAX_INSTANCES];

	current_apps[0].state = TS_APP_ACTIVE;
	uint8_t before = current_apps[0].active_slot;

	zassert_equal(ts_appmgr_rollback(), TS_E_IO, "空目标槽 = 拒绝回滚");
	zassert_equal(current_apps[0].active_slot, before, "不翻转");

	/* 双槽皆装（交替）→ 回滚验证通过 → 翻转 + 计数 */
	zassert_equal(ts_appmgr_install(fixture_pkg, sizeof(fixture_pkg),
					root_key, &info), TS_OK);
	zassert_equal(info.active_slot, 0, "次装 slot A（双有效槽）");
	current_apps[0].state = TS_APP_ACTIVE;
	for (int i = 1; i <= TS_APPMGR_ROLLBACK_LIMIT; i++) {
		zassert_equal(ts_appmgr_rollback(), TS_OK, "rollback %d（双有效槽）", i);
		zassert_equal(current_apps[0].rollback_count, i);
	}
	zassert_equal(ts_appmgr_rollback(), TS_E_ROLLBACK_LIMIT,
		      "rollback limit reached");
	ts_appmgr_get_info(&info);
	zassert_equal(info.state, TS_APP_QUARANTINED);
	zassert_equal(info.rollback_count, TS_APPMGR_ROLLBACK_LIMIT);
	zassert_equal(ts_appmgr_rollback(), TS_E_STATE, "quarantined: no further");
}

ZTEST(framework_appmgr, test_meta_persistence)
{
	/* 清 store → 写 meta → 读取 → 掉电模拟（reset + re-read） */
	ts_store_test_reset();
	ts_appmgr_meta_t meta = {.ver = TS_APPMGR_META_VER, .count = 1,
				 .entries = {{.active_slot = 1, .rollback_count = 2, .boot_gen = 7}}};

	zassert_equal(ts_appmgr_meta_write(&meta), TS_OK);

	ts_appmgr_meta_t rb;
	zassert_equal(ts_appmgr_meta_read(&rb), TS_OK);
	zassert_equal(rb.entries[0].active_slot, 1);
	zassert_equal(rb.entries[0].rollback_count, 2);
	zassert_equal(rb.entries[0].boot_gen, 7);

	/* meta 摘要随载（DEC-49②） */
	ts_app_info_t info;

	zassert_equal(ts_appmgr_install(fixture_pkg, sizeof(fixture_pkg),
					root_key, &info), TS_OK);
	ts_appmgr_meta_t m2;

	zassert_equal(ts_appmgr_meta_read(&m2), TS_OK);
	uint8_t hdr_digest[32];

	zassert_equal(ts_store_slot_read(info.active_slot, TSAP_DIGEST_OFF,
					 hdr_digest, 32), TS_OK);
	zassert_equal(memcmp(m2.entries[0].content_digest, hdr_digest, 32), 0,
		      "meta 摘要 == 头摘要");
}

ZTEST(framework_appmgr, test_fresh_system_no_app)
{
	/* IR2-07：空板（无有效 meta）= meta_read 如实 E_IO（不静默伪造成
	 * 有效态）；观测面 get_info 仍给缺省可观测态。
	 * 执行序自足化：先清 store（前序用例可能已写 meta）。 */
	ts_store_test_reset();
	ts_appmgr_test_reset();
	ts_appmgr_meta_t meta;

	zassert_equal(ts_appmgr_meta_read(&meta), TS_E_IO, "空板 = E_IO（如实）");
	ts_app_info_t info;

	zassert_equal(ts_appmgr_get_info(&info), TS_OK);
	zassert_equal(info.active_slot, 0, "缺省 slot 0（可观测）");
	zassert_equal(info.state, TS_APP_STAGED, "no app: staged default");
	/* boot：meta 不可读 → 不装载（IR2-07：不静默装 slot 0） */
	zassert_equal(ts_appmgr_boot_start(), TS_E_STATE, "boot 拒绝静默装载");
}

ZTEST(framework_appmgr, test_boot_quarantine_refusal)
{
	ts_store_test_reset(); /* 执行序自足化（fresh 前置条件）*/
	/* G4（单元 H）：隔离拒载 boot 面。判据以返回码区分：计数 == LIMIT
	 * （第 3 次回滚落地态）→ 放行过隔离门（本套件 fixture wasm 非真字节
	 * → 装载失败 E_IO = 已达装载路径的证明）；标记 LIMIT+1 → E_ROLLBACK_
	 * LIMIT（门内即拒，未触装载）+ 观测面 QUARANTINED。
	 * 本用例字典序居首——收尾复原空板（后续用例此前依赖套件级 setup 的
	 * 处女 store）。 */
	ts_store_test_reset();
	ts_appmgr_test_reset();
	ts_app_info_t info;

	zassert_equal(ts_appmgr_install(fixture_pkg, sizeof(fixture_pkg),
					root_key, &info), TS_OK);

	ts_appmgr_meta_t meta;

	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	meta.entries[0].rollback_count = TS_APPMGR_ROLLBACK_LIMIT;
	zassert_equal(ts_appmgr_meta_write(&meta), TS_OK);
	zassert_equal(ts_appmgr_boot_start(), TS_E_IO,
		      "count=LIMIT：放行至装载（fixture wasm 非真 → E_IO）");

	meta.entries[0].rollback_count = TS_APPMGR_ROLLBACK_LIMIT + 1;
	zassert_equal(ts_appmgr_meta_write(&meta), TS_OK);
	zassert_equal(ts_appmgr_boot_start(), TS_E_ROLLBACK_LIMIT, "隔离拒载");
	zassert_equal(ts_appmgr_get_info(&info), TS_OK);
	zassert_equal(info.state, TS_APP_QUARANTINED, "观测面终态如实");
	zassert_equal(info.rollback_count, TS_APPMGR_ROLLBACK_LIMIT,
		      "运行时计数如实（meta 标记 = LIMIT+1 内部判据）");

	/* rollback() 隔离分支持久化标记：count=LIMIT 起点 → meta 写 LIMIT+1 */
	ts_store_test_reset();
	ts_appmgr_test_reset();
	zassert_equal(ts_appmgr_install(fixture_pkg, sizeof(fixture_pkg),
					root_key, &info), TS_OK);
	zassert_equal(ts_appmgr_install(fixture_pkg, sizeof(fixture_pkg),
					root_key, &info), TS_OK, "双有效槽");
	extern ts_app_info_t current_apps[CONFIG_TS_APP_MAX_INSTANCES];

	current_apps[0].state = TS_APP_ACTIVE;
	current_apps[0].rollback_count = TS_APPMGR_ROLLBACK_LIMIT;
	zassert_equal(ts_appmgr_rollback(), TS_E_ROLLBACK_LIMIT);
	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.entries[0].rollback_count, TS_APPMGR_ROLLBACK_LIMIT + 1,
		      "隔离标记持久化（boot 拒载判据）");

	/* 收尾复原（处女 store 归还后续用例） */
	ts_store_test_reset();
	ts_appmgr_test_reset();
}

/* ---- MA3.1：分步安装链（stage_begin/chunk/verify/activate，LLD-agent-deploy-net §3）-- */

ZTEST(framework_appmgr, test_staged_install_chain)
{
	fx_fields();
	static uint8_t pkg[PKG_BUFFER_SIZE];

	memcpy(pkg, fixture_pkg, fx_total);
	ts_store_test_reset();

	/* 尺寸域：过小/过大拒绝 */
	zassert_equal(ts_appmgr_stage_begin(TSAP_HEADER_SIZE, NULL), TS_E_PARAM);
	zassert_equal(ts_appmgr_stage_begin(CONFIG_TS_STORE_SLOT_SIZE + 1, NULL),
		      TS_E_PARAM);

	/* 未 begin：chunk/verify/activate 全 STATE（fail-closed） */
	zassert_equal(ts_appmgr_stage_chunk(0, pkg, 8, NULL), TS_E_STATE);
	zassert_equal(ts_appmgr_stage_verify(root_key, NULL, NULL, NULL), TS_E_STATE);
	zassert_equal(ts_appmgr_stage_activate(NULL), TS_E_STATE);

	/* begin → slot B；分两半 chunk（断点续传语义） */
	uint8_t slot = 9;
	uint32_t hw = 0;

	zassert_equal(ts_appmgr_stage_begin(fx_total, &slot), TS_OK);
	zassert_equal(slot, 1, "inactive = B（active=0^1）");

	zassert_equal(ts_appmgr_stage_chunk(fx_total, pkg, 4, NULL), TS_E_PARAM,
		      "越界 chunk 拒绝");

	zassert_equal(ts_appmgr_stage_chunk(0, pkg, fx_total / 2, &hw), TS_OK);
	zassert_equal(hw, fx_total / 2);
	zassert_equal(ts_appmgr_stage_verify(root_key, NULL, NULL, NULL), TS_E_STATE,
		      "未收满：verify 拒绝");

	zassert_equal(ts_appmgr_stage_chunk(fx_total / 2, pkg + fx_total / 2,
					    fx_total - fx_total / 2, &hw), TS_OK);
	zassert_equal(hw, fx_total, "high_water = total");

	zassert_equal(ts_appmgr_stage_activate(NULL), TS_E_STATE,
		      "未验证：activate 拒绝");

	/* verify：容器事实对拍（ml/wl 与生成期头一致；cose_off = 48+ml+wl） */
	uint32_t ml = 0, wl = 0, co = 0;

	zassert_equal(ts_appmgr_stage_verify(root_key, &ml, &wl, &co), TS_OK,
		      "真验签通过（测试根）");
	zassert_equal(ml, fx_ml);
	zassert_equal(wl, fx_wl);
	zassert_equal(co, TSAP_CONTENT_OFF + fx_ml + fx_wl);

	ts_app_info_t info;

	zassert_equal(ts_appmgr_stage_activate(&info), TS_OK);
	zassert_equal(info.state, TS_APP_STAGED);
	zassert_equal(info.active_slot, 1);
	zassert_equal(ts_appmgr_stage_activate(NULL), TS_E_STATE, "终态清台");
	zassert_equal(ts_appmgr_stage_chunk(0, pkg, 8, NULL), TS_E_STATE);

	uint8_t rb[4];

	zassert_equal(ts_store_slot_read(1, 0, rb, 4), TS_OK);
	zassert_equal(rb[0], 'T');
}

/* ---- 密码件基线：sha256 NIST 向量（store/sha256.c 实现护栏；DEC-49 复用）-- */
#include "../../../module/tessera/src/store/internal.h"

ZTEST(framework_appmgr, test_sha256_vectors)
{
	/* FIPS 180-4 标准向量 */
	static const uint8_t vec_abc[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
		0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
		0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	static const uint8_t vec_empty[32] = {
		0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
		0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
		0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
	};
	uint8_t out[32];
	ts_sha256_ctx_t c;

	ts_sha256_init(&c);
	ts_sha256_update(&c, (const uint8_t *)"abc", 3);
	ts_sha256_final(&c, out);
	zassert_equal(memcmp(out, vec_abc, 32), 0, "sha256(abc)");

	ts_sha256_init(&c);
	ts_sha256_final(&c, out);
	zassert_equal(memcmp(out, vec_empty, 32), 0, "sha256(empty)");

	/* 流式 = 一次等价（21B 分三段 7B 喂入，跨块边界语义） */
	static const uint8_t msg21[21] = "abcabcabcabcabcabcabc";

	ts_sha256_init(&c);
	for (int i = 0; i < 3; i++) {
		ts_sha256_update(&c, msg21 + i * 7, 7);
	}
	ts_sha256_final(&c, out);
	uint8_t ref[32];
	ts_sha256_ctx_t c2;

	ts_sha256_init(&c2);
	ts_sha256_update(&c2, msg21, sizeof(msg21));
	ts_sha256_final(&c2, ref);
	zassert_equal(memcmp(out, ref, 32), 0, "流式一致性");
}

/* ---- 密码件基线：tweetnacl 内建自测（已知向量 sm/pub → crypto_sign_open）
 * ——模块构建下的库护栏（调试期别名缺陷即由本面拦截路径演化而来）-------- */
ZTEST(framework_appmgr, test_tnacl_selftest)
{
	extern int ts_tnacl_selftest(void);

	zassert_equal(ts_tnacl_selftest(), 0, "tweetnacl 已知向量验签");
}

ZTEST_SUITE(framework_appmgr, NULL, appmgr_setup, NULL, NULL, NULL);
