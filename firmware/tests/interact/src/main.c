/* SPDX-License-Identifier: Apache-2.0 */
/* ts-interact L1 测试（LLD-ts-interact §5）：核心逻辑直测（shell 运行时外）——
 *   test_01 confirm/reject 域：无 pending = E_STATE；pending 置入后确认
 *           rc=0 + meta 切换、再确认 E_STATE；对称拒绝（槽保留可重推）；
 *   test_02 arb/estop：席位双向切换（幂等）；锁存释放幂等（框架契约恒
 *           TS_OK）+ force 后清除迁移出 FAULT；
 *   test_03 log 级别：五档全过 + 域外拒 + NULL 拒。
 * 夹具复用 app 套件机械生成物（boot_pkg.h）与 prov12/stage 形态（app 套件
 * 同源——避免生成器分叉）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include <ts/store.h>
#include "../../../module/tessera/src/interact/ts_interact.h"
#include "../../app/src/boot_pkg.h"

static size_t build_prov12(uint8_t *o, size_t cap, uint8_t lconf)
{
	/* 容量契约（R5 批实测 204B——12 对；此前 ARG_UNUSED(cap) + 调用方
	 * pb[192] 构成 12B 栈越界写存量缺陷，两侧套件同修）：不足即拒。 */
	if (cap < 256) {
		return 0;
	}
	size_t n = 0;

	o[n++] = 0xAC; /* map(12) */
#define P_T(s) do { o[n++] = (uint8_t)(0x60 | (sizeof(s) - 1)); \
	memcpy(o + n, s, sizeof(s) - 1); n += sizeof(s) - 1; } while (0)
#define P_KV_U(k, v) do { P_T(k); o[n++] = (uint8_t)(v); } while (0)
	P_KV_U("v", 2);
	P_T("node_id"); P_T("gate-n");
	P_T("cube_id"); P_T("gate-c");
	P_T("routers"); o[n++] = 0x81; P_T("tcp/10.0.0.9:7447");
	P_T("pk0"); o[n++] = 0x58; o[n++] = 32; memset(o + n, 0x11, 32); n += 32;
	P_T("pk1"); o[n++] = 0x58; o[n++] = 32; memset(o + n, 0x22, 32); n += 32;
	P_T("cred"); P_T("cred");
	P_T("pwr_ma"); o[n++] = 0x19; o[n++] = 500 >> 8; o[n++] = 500 & 0xFF;
	P_KV_U("estop", 0);
	P_KV_U("deploy_mode", 0);
	P_KV_U("estop_remote_clear", 1);
	P_KV_U("lconf", lconf);
#undef P_KV_U
#undef P_T
	return n;
}

static ts_res_t stage_full(uint8_t *slot_out)
{
	uint8_t slot;
	uint32_t hw;
	ts_res_t r = ts_appmgr_stage_begin(sizeof(boot_pkg), &slot);

	if (r != TS_OK) {
		return r;
	}
	r = ts_appmgr_stage_chunk(0, boot_pkg, sizeof(boot_pkg), &hw);
	if (r == TS_OK && hw == sizeof(boot_pkg)) {
		r = ts_appmgr_stage_verify(NULL, NULL, NULL, NULL);
	}
	if (slot_out != NULL) {
		*slot_out = slot;
	}
	return r;
}

/* pending 置入：lconf=1 prov + activate 过闸（app 套件 test_14 同源） */
static ts_res_t put_pending(uint8_t *slot_out)
{
	uint8_t pb[256];
	ts_app_info_t info;
	uint8_t slot;

	ts_store_test_reset();
	ts_appmgr_test_reset();
	ts_safety_test_reset();
	ts_safety_set_arb(TS_ARB_REMOTE);
	size_t pn = build_prov12(pb, sizeof(pb), 1);
	ts_res_t r = ts_store_prov_write_test(pb, pn);

	if (r != TS_OK) {
		return r;
	}
	r = ts_store_prov_load();
	if (r != TS_OK) {
		return r;
	}
	/* 直装基线版（闸同过——EC4 唯一收口），meta 就位 */
	r = ts_appmgr_install(boot_pkg, sizeof(boot_pkg), NULL, &info);
	if (r == TS_E_PENDING) {
		r = ts_appmgr_confirm_pending(&info);
	}
	if (r != TS_OK) {
		return r;
	}
	/* 新版本 activate → pending */
	r = stage_full(&slot);
	if (r != TS_OK) {
		return r;
	}
	r = ts_appmgr_stage_activate(&info);
	if (r != TS_E_PENDING) {
		return r != TS_OK ? r : TS_E_STATE;
	}
	if (slot_out != NULL) {
		*slot_out = slot;
	}
	return TS_OK;
}

ZTEST(framework_interact, test_01_confirm_reject_domain)
{
	ts_app_info_t info;

	/* 无 pending：确认/拒绝 = E_STATE（非 pending 期手势模态外同语义） */
	ts_store_test_reset();
	ts_appmgr_test_reset();
	zassert_equal(ts_interact_confirm(&info), TS_E_STATE);
	zassert_equal(ts_interact_reject(), TS_E_STATE);

	/* pending 置入 → ts confirm：meta 切换 + 槽就位（终端 = BOOT 短按） */
	uint8_t pslot;

	zassert_equal(put_pending(&pslot), TS_OK);
	zassert_true(ts_appmgr_pending_active());
	zassert_equal(ts_interact_confirm(&info), TS_OK, "终端确认过闸");
	zassert_false(ts_appmgr_pending_active());
	ts_appmgr_meta_t meta;

	zassert_equal(ts_appmgr_meta_read(&meta), TS_OK);
	zassert_equal(meta.entries[0].active_slot, pslot,
		      "确认后 meta 已指向挂起槽（= 手势确认同语义）");
	zassert_equal(ts_interact_confirm(&info), TS_E_STATE, "已消费不复确认");

	/* 对称拒绝：pending 置入 → ts reject（槽保留可重推） */
	zassert_equal(put_pending(NULL), TS_OK);
	zassert_equal(ts_interact_reject(), TS_OK, "终端拒绝过闸");
	zassert_false(ts_appmgr_pending_active());
	zassert_equal(ts_interact_reject(), TS_E_STATE);
	/* 拒绝后槽保留 → 重推同名包 verify 过（appmgr 台面已清可重 staging） */
	uint8_t slot;

	zassert_equal(stage_full(&slot), TS_OK, "拒绝后槽可重推");
}

ZTEST(framework_interact, test_02_arb_estop_domain)
{
	ts_store_test_reset();
	ts_appmgr_test_reset();
	ts_safety_test_reset();
	zassert_equal(ts_safety_set_arb(TS_ARB_REMOTE), TS_OK);

	/* CG3 席位：终端与 BOOT 双短按同入口（幂等 + 切换零输出） */
	zassert_equal(ts_interact_arb(true), TS_OK);
	zassert_equal(ts_safety_arb_owner(), TS_ARB_LOCAL);
	zassert_equal(ts_interact_arb(true), TS_OK, "幂等（同席重设）");
	zassert_equal(ts_interact_arb(false), TS_OK);
	zassert_equal(ts_safety_arb_owner(), TS_ARB_REMOTE);

	/* F2 本地清除：框架契约 = 锁存释放恒 TS_OK（幂等；无 fault = no-op） */
	zassert_equal(ts_interact_estop_clear(), TS_OK, "锁存释放幂等");
	const ts_out_ch_t ch = {
		.uid = "tsi_probe", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = true},
	};
	zassert_equal(ts_safety_register_channel(&ch), TS_OK);
	/* force 通道入 SAFE_FAULT → 本地清除迁移出 FAULT（AKF/AKC 同款；
	 * commit 值 == fault 值 = SAFE_FAULT 入态路径，不经 commit） */
	zassert_equal(ts_safety_force_channel_fault("tsi_probe"), TS_OK);
	zassert_equal(ts_interact_estop_clear(), TS_OK, "fault 清除过闸");
	ts_ch_state_t st;

	zassert_equal(ts_safety_channel_state("tsi_probe", &st), TS_OK);
	zassert_not_equal(st, TS_ST_SAFE_FAULT, "已迁移出 FAULT");
}

ZTEST(framework_interact, test_03_log_level_domain)
{
	static const char *const ok[] = {"off", "err", "wrn", "inf", "dbg"};

	for (int i = 0; i < 5; i++) {
		zassert_equal(ts_interact_log_level(ok[i]), TS_OK);
	}
	zassert_equal(ts_interact_log_level("verbose"), TS_E_PARAM, "域外拒");
	zassert_equal(ts_interact_log_level(""), TS_E_PARAM);
	zassert_equal(ts_interact_log_level(NULL), TS_E_PARAM);
	printk("INTERACT DONE\n"); /* harness 判据行（testcase.yaml） */
}

ZTEST_SUITE(framework_interact, NULL, NULL, NULL, NULL, NULL);
