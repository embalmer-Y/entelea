/* SPDX-License-Identifier: Apache-2.0 */
/* r16 D12 闸 ztest：fail-closed、频度 6/60s、E_BUSY 冷却 60s、风暴 3 闩
 * +外部 clear、异常复位冷却窗、观测计数。 */
#include <zephyr/ztest.h>
#include <entelea/d12.h>
#include <entelea/time.h>

static uint64_t fake_now;
static entelea_clock_t clk;

static entelea_result_t fake_read(void *user, uint64_t *out)
{
	*out = *(uint64_t *)user;
	return ENTELEA_OK;
}

static void bind(uint64_t t0)
{
	fake_now = t0;
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
