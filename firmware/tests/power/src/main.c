/* SPDX-License-Identifier: Apache-2.0 */
/* ts-power L1/L2 测试（LLD-ts-power §6）：预算记账数学（边界：恰等/超 1mA）/
 * 限流拒绝 / 断链→供电槽 linkloss / 权限门。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/power.h>
#include <ts/safety.h>

/* 权限 ctx：power:set 全实例 */
static ts_ctx_t make_ctx(void)
{
	ts_ctx_t ctx;
	ts_perm_table_t t;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("power:set:0-3", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 7, &t), TS_OK);
	return ctx;
}

static int budget_evt_seen;

static void on_budget_evt(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(u);
	ARG_UNUSED(e);
	budget_evt_seen++;
}

ZTEST(framework_power, test_01_budget_math)
{
	static const ts_pwr_slot_t s0 = {.uid = "pb0", .current_limit_ma = 250};
	static const ts_pwr_slot_t s1 = {.uid = "pb1", .current_limit_ma = 200};
	ts_ctx_t ctx = make_ctx();

	ts_safety_test_reset();
	ts_power_test_reset();
	zassert_equal(ts_power_register_slot(&s0), TS_OK);
	zassert_equal(ts_power_register_slot(&s1), TS_OK);
	zassert_equal(ts_power_slot_count(), 2);
	ts_power_test_set_budget(200);
	ts_safety_set_link(true); /* 供电提交须 ACTIVE 态 */

	/* 限额内开 150mA */
	zassert_equal(ts_power_request(ctx, 0, true, 150), TS_OK);
	ts_power_budget_t b;
	ts_power_budget_snapshot(&b);
	zassert_equal(b.used_ma, 150, "用量记账");

	/* 超预算：150+100 > 200 → RANGE + 事件 */
	budget_evt_seen = 0;
	zassert_equal(ts_evt_subscribe(TS_EVT_POWER_BUDGET, on_budget_evt, NULL), TS_OK);
	zassert_equal(ts_power_request(ctx, 1, true, 100), TS_E_RANGE);
	zassert_equal(budget_evt_seen, 1, "超预算事件外发");
	/* 既有槽不受影响（不在线降额） */
	ts_power_budget_snapshot(&b);
	zassert_equal(b.used_ma, 150, "既有槽不受影响");

	/* 边界：恰好等于预算 → OK；超 1mA → 拒 */
	zassert_equal(ts_power_request(ctx, 0, true, 200), TS_OK, "恰等于预算");
	zassert_equal(ts_power_request(ctx, 0, true, 201), TS_E_RANGE, "超 1mA 拒绝");

	/* 峰值跟踪；关断释放预算 */
	ts_power_budget_snapshot(&b);
	zassert_equal(b.peak_ma, 200, "峰值");
	zassert_equal(ts_power_request(ctx, 0, false, 0), TS_OK);
	ts_power_budget_snapshot(&b);
	zassert_equal(b.used_ma, 0, "关断释放");

	/* 限流（通道 current_limit 兜底）：预算放大到 1000 后请求 300 > 250 → RANGE */
	ts_power_test_set_budget(1000);
	zassert_equal(ts_power_request(ctx, 0, true, 300), TS_E_RANGE, "硬限流拒绝");
}

ZTEST(framework_power, test_02_poweron_and_linkloss)
{
	static const ts_pwr_slot_t on_slot = {.uid = "pb_on", .current_limit_ma = 100,
					      .poweron_on = true};
	static const ts_pwr_slot_t off_slot = {.uid = "pb_off", .current_limit_ma = 100};
	ts_out_value_t rb;

	ts_safety_test_reset();
	ts_power_test_reset();
	zassert_equal(ts_power_register_slot(&on_slot), TS_OK);
	zassert_equal(ts_power_register_slot(&off_slot), TS_OK);

	/* 上电态：poweron_on 槽供电（ma = limit，保守口径）；off 槽关断 */
	zassert_equal(ts_safety_readback("pb_on", &rb), TS_OK);
	zassert_true(rb.pwr.en && rb.pwr.ma == 100, "上电态按声明供电");
	zassert_equal(ts_safety_readback("pb_off", &rb), TS_OK);
	zassert_false(rb.pwr.en, "上电态缺省关断");

	/* 链路确立（POWERON → ACTIVE）后断链 → 供电槽进 linkloss 态
	 *（= 关断，本模块生成的安全侧缺省；POWERON 不因断链迁移——LLD §3 图） */
	ts_safety_set_link(true);
	ts_ch_state_t st;

	zassert_equal(ts_safety_channel_state("pb_on", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE);
	ts_safety_set_link(false);

	zassert_equal(ts_safety_channel_state("pb_on", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_LINKLOSS);
	zassert_equal(ts_safety_readback("pb_on", &rb), TS_OK);
	zassert_false(rb.pwr.en, "断链关断（合同 3/7）");

	/* 恢复 → ACTIVE（DR-04：不自动回写断链前的值） */
	ts_safety_set_link(true);
	zassert_equal(ts_safety_channel_state("pb_on", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE);
	zassert_equal(ts_safety_readback("pb_on", &rb), TS_OK);
	zassert_false(rb.pwr.en, "恢复后保持关断直到显式请求");
}

ZTEST(framework_power, test_03_param_and_perm)
{
	ts_ctx_t ctx = make_ctx();
	static const ts_pwr_slot_t s = {.uid = "pb_p", .current_limit_ma = 100};

	ts_safety_test_reset();
	ts_power_test_reset();
	zassert_equal(ts_power_register_slot(NULL), TS_E_PARAM);
	zassert_equal(ts_power_register_slot(
		&((ts_pwr_slot_t){.uid = "x", .current_limit_ma = 0})), TS_E_PARAM,
		"零限流拒绝");
	zassert_equal(ts_power_register_slot(&s), TS_OK);
	zassert_equal(ts_power_register_slot(&s), TS_E_PARAM, "撞 uid 拒绝");

	/* 未授权 ctx（gpio only）→ TS_E_PERM */
	ts_ctx_t rogue;
	ts_perm_table_t t;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("gpio:write:0", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&rogue, 9, &t), TS_OK);
	zassert_equal(ts_power_request(rogue, 0, true, 50), TS_E_PERM, "权限门");

	/* 未注册槽 */
	zassert_equal(ts_power_request(ctx, 3, true, 50), TS_E_NOTFOUND);
}

ZTEST_SUITE(framework_power, NULL, NULL, NULL, NULL, NULL);
