/* SPDX-License-Identifier: Apache-2.0 */
/* PWM 真后端路径测试（review 缺陷 2）：
 * - u==0 停止语义（占空 0）——修复前 hz==0 被当非法拒绝落驱，fault 值 0
 *   物理上保持原输出；
 * - 后端失败传播：pwm_set 失败 → commit E_IO、影子/golden 不更新（修复前
 *   返回成功且 shadow 已更新）；
 * - poweron 后端失败 → E_IO（boot 将 fail-safe）；force 路径失败计数。
 * 假设备 cycles_per_sec = 1：捕获值即 ns。 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/safety.h>

static uint32_t cap_period;
static uint32_t cap_pulse;
static bool pwm_fail_next;
static int set_calls;

static int fake_set_cycles(const struct device *dev, uint32_t channel,
			   uint32_t period_cycles, uint32_t pulse_cycles,
			   pwm_flags_t flags)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(flags);
	set_calls++;
	if (pwm_fail_next) {
		pwm_fail_next = false;
		return -EIO;
	}
	cap_period = period_cycles;
	cap_pulse = pulse_cycles;
	return 0;
}

static int fake_get_cycles_per_sec(const struct device *dev, uint32_t channel,
				   uint64_t *rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	*rate = 1000000000ULL; /* cycles = ns * rate / 1e9 = ns */
	return 0;
}

static DEVICE_API(pwm, fake_pwm_api) = {
	.set_cycles = fake_set_cycles,
	.get_cycles_per_sec = fake_get_cycles_per_sec,
};

DEVICE_DT_DEFINE(DT_NODELABEL(fake_pwm), NULL, NULL, NULL, NULL, POST_KERNEL,
		CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_pwm_api);

static const entelea_out_channel_t bus_ch = {
	.uid = "pwmbus", .kind = ENTELEA_CH_PWM,
	.poweron = {.u = 0}, .linkloss = {.u = 0}, .fault = {.u = 0},
	.limits = {.min = 0, .max = 0xFFFFFFFF},
};

/* 打包：高16=hz/100、低16=permille。 */
#define PACK(hz, permille) ((((hz) / 100) << 16) | (permille))

ZTEST(framework_safety_pwm, test_pwm_zero_value_stops_output)
{
	entelea_safety_test_reset();
	zassert_equal(entelea_drv_pwm_init(), 0, "fake PWM ready");

	set_calls = 0;
	zassert_equal(entelea_safety_register_channel(&bus_ch), ENTELEA_OK);
	/* poweron 值 u=0：解码为停止（pulse=0），修复前该写被拒绝。 */
	zassert_equal(entelea_safety_poweron_init(), ENTELEA_OK);
	zassert_true(set_calls > 0, "zero poweron value must reach the backend");
	zassert_equal(cap_pulse, 0U, "u==0 = duty zero (physical stop)");
	zassert_equal(cap_period, 1000000000U, "stop uses the fixed 1s period");

	/* 激活后显式提交 u=0 同样停止。 */
	entelea_safety_set_link(true);
	entelea_out_value_t rb;

	zassert_equal(entelea_safety_commit("pwmbus", (entelea_out_value_t){.u = 0}),
		      ENTELEA_OK);
	zassert_equal(entelea_safety_readback("pwmbus", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 0U);
	zassert_equal(cap_pulse, 0U, "explicit zero commit = stop");

	/* 正常值换算：1000Hz × 50%。 */
	zassert_equal(entelea_safety_commit("pwmbus",
					    (entelea_out_value_t){.u = PACK(1000, 500)}),
		      ENTELEA_OK);
	zassert_equal(cap_period, 1000000U, "1e9/1000Hz");
	zassert_equal(cap_pulse, 500000U, "50% duty");
	/* force（fault=0）→ 物理回到停止。 */
	entelea_safety_force_all_fault();
	zassert_equal(cap_pulse, 0U, "fault value 0 = physical stop");
}

ZTEST(framework_safety_pwm, test_backend_failure_never_reports_success)
{
	entelea_safety_test_reset();
	zassert_equal(entelea_drv_pwm_init(), 0);
	zassert_equal(entelea_safety_register_channel(&bus_ch), ENTELEA_OK);
	entelea_safety_set_link(true);
	zassert_equal(entelea_safety_commit("pwmbus",
					    (entelea_out_value_t){.u = PACK(1000, 100)}),
		      ENTELEA_OK);
	entelea_out_value_t rb;

	zassert_equal(entelea_safety_readback("pwmbus", &rb), ENTELEA_OK);
	uint32_t good = rb.u;
	entelea_write_rec_t recs[8];
	size_t writes = entelea_driver_sim_writes("pwmbus", recs, 8);

	pwm_fail_next = true;
	zassert_equal(entelea_safety_commit("pwmbus",
					    (entelea_out_value_t){.u = PACK(1000, 900)}),
		      ENTELEA_E_IO, "backend failure must propagate");
	zassert_equal(entelea_safety_readback("pwmbus", &rb), ENTELEA_OK);
	zassert_equal(rb.u, good, "shadow must not advance on failure");
	size_t after = entelea_driver_sim_writes("pwmbus", recs, 8);

	zassert_equal(after, writes, "no golden record for a failed write");
	zassert_true(entelea_safety_force_write_failures() >= 1,
		     "failure observable via counter");

	/* poweron 路径后端失败 → E_IO（boot 将 fail-safe）。 */
	entelea_safety_test_reset();
	zassert_equal(entelea_drv_pwm_init(), 0);
	zassert_equal(entelea_safety_register_channel(&bus_ch), ENTELEA_OK);
	pwm_fail_next = true;
	zassert_equal(entelea_safety_poweron_init(), ENTELEA_E_IO,
		      "poweron backend failure must fail boot step");

	/* force 路径（ISR 语义）失败不可返回，但必须计数留痕。 */
	uint32_t fails = entelea_safety_force_write_failures();

	pwm_fail_next = true;
	entelea_safety_force_all_fault();
	zassert_true(entelea_safety_force_write_failures() > fails,
		     "force-path write failure counted");
}

ZTEST_SUITE(framework_safety_pwm, NULL, NULL, NULL, NULL, NULL);
