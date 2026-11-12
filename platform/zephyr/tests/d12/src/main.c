/* SPDX-License-Identifier: Apache-2.0 */
/* r16 D12 闸 ztest：fail-closed、频度 6/60s、E_BUSY 冷却 60s、风暴 3 闩
 * +外部 clear、异常复位冷却窗、观测计数；RF1 G5：t=0 槽、时钟回退、
 * 时钟失明期冷却不丢、重绑时钟不清风暴闩。 */
#include <zephyr/ztest.h>
#include <entelea/d12.h>
#include <entelea/time.h>

static uint64_t fake_now;
static bool fake_fail;
static entelea_clock_t clk;

static entelea_result_t fake_read(void *user, uint64_t *out)
{
	if (fake_fail) {
		return ENTELEA_E_IO;
	}
	*out = *(uint64_t *)user;
	return ENTELEA_OK;
}

static void bind(uint64_t t0)
{
	fake_now = t0;
	fake_fail = false;
	entelea_d12_test_reset();
	zassert_ok(entelea_clock_init(&clk, fake_read, &fake_now, 1));
	zassert_ok(entelea_d12_bind_clock(&clk));
}

ZTEST_SUITE(framework_d12, NULL, NULL, NULL, NULL, NULL);

ZTEST(framework_d12, test_fail_closed_without_clock)
{
	entelea_d12_test_reset();
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_STATE);
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_E_STATE);
}

ZTEST(framework_d12, test_rate_window_and_lifetime)
{
	bind(1000);
	for (int i = 0; i < 6; i++) {
		zassert_equal(entelea_d12_model_allow(), ENTELEA_OK);
	}
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY,
		      "第 7 次（60s 窗内）拒");
	fake_now += 61000; /* 出窗 */
	zassert_equal(entelea_d12_model_allow(), ENTELEA_OK);
	zassert_equal(entelea_d12_model_blocked(), 1);
}

ZTEST(framework_d12, test_busy_cooldown)
{
	bind(1000);
	zassert_equal(entelea_d12_model_allow(), ENTELEA_OK);
	entelea_d12_model_note(ENTELEA_E_BUSY);
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY, "冷却内拒");
	fake_now += 61000;
	zassert_equal(entelea_d12_model_allow(), ENTELEA_OK, "冷却出");
}

ZTEST(framework_d12, test_storm_latch_and_clear)
{
	bind(1000);
	entelea_d12_model_note(ENTELEA_E_IO);
	entelea_d12_model_note(ENTELEA_E_IO);
	zassert_false(entelea_d12_storm_latched(), "2 败未闩");
	entelea_d12_model_note(ENTELEA_E_IO);
	zassert_true(entelea_d12_storm_latched(), "3 连败闩");
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_STATE, "闩内拒");
	entelea_d12_model_note(ENTELEA_OK); /* 闩内成功上报不自动解 */
	zassert_true(entelea_d12_storm_latched());
	entelea_d12_storm_clear();
	zassert_equal(entelea_d12_model_allow(), ENTELEA_OK, "外部释放");
}

ZTEST(framework_d12, test_boot_cooldown_covers_wasm)
{
	bind(1000);
	entelea_d12_boot_note(true); /* 异常复位 */
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_E_BUSY, "冷却内拒");
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY);
	fake_now += 61000;
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_OK);
	entelea_d12_boot_note(false); /* 正常复位无窗 */
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_OK);
}

/* RF1 G5（审计 #11）：t=0 发生的调用必须占窗（旧实现 window==0 当空槽）。 */
ZTEST(framework_d12, test_rate_counts_calls_at_t_zero)
{
	bind(0);
	for (int i = 0; i < 6; i++) {
		zassert_equal(entelea_d12_model_allow(), ENTELEA_OK);
	}
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY,
		      "t=0 的 6 次已占满 60s 窗");
	zassert_equal(entelea_d12_model_calls(), 6);
}

/* RF1 G5：时钟回退不得经无符号下溢放行（单调夹紧，窗/冷却不缩短）。 */
ZTEST(framework_d12, test_clock_regression_keeps_window)
{
	bind(100000);
	for (int i = 0; i < 6; i++) {
		zassert_equal(entelea_d12_model_allow(), ENTELEA_OK);
	}
	fake_now = 50000; /* 回退 */
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY, "回退不放行");
	fake_now = 159999;
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY, "窗未出");
	fake_now = 160000;
	zassert_equal(entelea_d12_model_allow(), ENTELEA_OK, "出窗");
}

/* RF1 G5：异常复位上报时时钟失明——冷却挂起至首次好读起算，不丢。 */
ZTEST(framework_d12, test_boot_cooldown_survives_clock_fault)
{
	bind(1000);
	fake_fail = true;
	entelea_d12_boot_note(true);
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_E_STATE, "失明闭锁");
	fake_fail = false;
	fake_now = 5000;
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_E_BUSY, "冷却补立");
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY);
	fake_now = 5000 + 59999;
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_E_BUSY);
	fake_now = 5000 + 60000;
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_OK);
}

/* RF1 G5：E_BUSY 上报时时钟失明——429 冷却同样挂起不丢。 */
ZTEST(framework_d12, test_busy_cooldown_survives_clock_fault)
{
	bind(1000);
	zassert_equal(entelea_d12_model_allow(), ENTELEA_OK);
	fake_fail = true;
	entelea_d12_model_note(ENTELEA_E_BUSY);
	fake_fail = false;
	fake_now = 2000;
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY, "冷却补立");
	fake_now = 2000 + 60000;
	zassert_equal(entelea_d12_model_allow(), ENTELEA_OK);
}

ZTEST(framework_d12, test_boot_cooldown_does_not_wrap_at_clock_limit)
{
	bind(UINT64_MAX - 1000);
	entelea_d12_boot_note(true);
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_E_BUSY);
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY);
	fake_now = UINT64_MAX;
	zassert_equal(entelea_d12_wasm_allow(), ENTELEA_E_BUSY);
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY);
}

ZTEST(framework_d12, test_busy_cooldown_does_not_wrap_at_clock_limit)
{
	bind(UINT64_MAX - 1000);
	zassert_ok(entelea_d12_model_allow());
	entelea_d12_model_note(ENTELEA_E_BUSY);
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY);
	fake_now = UINT64_MAX;
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_BUSY);
}

/* RF1 G5：重绑时钟不是风暴释放面（仅 storm_clear）。 */
ZTEST(framework_d12, test_storm_survives_clock_rebind)
{
	bind(1000);
	for (int i = 0; i < 3; i++) {
		entelea_d12_model_note(ENTELEA_E_IO);
	}
	zassert_true(entelea_d12_storm_latched());
	zassert_ok(entelea_d12_bind_clock(&clk));
	zassert_true(entelea_d12_storm_latched(), "重绑不清闩");
	zassert_equal(entelea_d12_model_allow(), ENTELEA_E_STATE);
}

/* RF1 G5：跨复位风暴判定——POR 起第 3 次连续异常复位即 SAFE-HALT
 * （≥3，非 >3）；纯 POR 清零；计数饱和。 */
ZTEST(framework_d12, test_reset_guard_threshold_and_por)
{
	uint32_t c = 0;

	zassert_false(entelea_d12_reset_guard(false, true, &c));
	zassert_equal(c, 0);
	zassert_false(entelea_d12_reset_guard(true, true, &c));
	zassert_false(entelea_d12_reset_guard(true, true, &c));
	zassert_true(entelea_d12_reset_guard(true, true, &c), "第 3 次即停");
	zassert_equal(c, ENTELEA_D12_RESET_STORM_MAX);
	zassert_true(entelea_d12_reset_guard(true, true, &c));
	zassert_equal(c, ENTELEA_D12_RESET_STORM_MAX, "饱和不回绕");
	zassert_false(entelea_d12_reset_guard(false, true, &c));
	zassert_equal(c, 0, "POR 出风暴窗");
}

/* RF1 G5：异常复位且留存历史不可知 → fail-closed；POR 时不可知
 * 不闭锁（POR 本身即出窗）。 */
ZTEST(framework_d12, test_reset_guard_unknown_history_fails_closed)
{
	uint32_t c = 0;

	zassert_true(entelea_d12_reset_guard(true, true, NULL));
	zassert_true(entelea_d12_reset_guard(true, false, &c));
	zassert_equal(c, ENTELEA_D12_RESET_STORM_MAX);
	c = 0xdeadbeef;
	zassert_false(entelea_d12_reset_guard(false, false, &c));
	zassert_equal(c, 0);
}
