/* SPDX-License-Identifier: Apache-2.0 */
/* R02 硬件看门狗后端负测与 boot 失败集成（迁自旧
 * firmware/tests/core/src/wdt_hardware.c）：后端失败不报告成功、失败
 * boot 不执行后续步骤、故障不等待普通执行队列（boot 线程停机挂起）。 */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"

static int install_result;
static int setup_result;
static int install_calls;
static int setup_calls;
static int feed_calls;
static struct wdt_timeout_cfg installed;
static uint8_t setup_options;

static int fake_init(const struct device *dev)
{
	ARG_UNUSED(dev);
#ifdef TEST_WDT_HW_NOT_READY
	return -ENODEV;
#else
	return 0;
#endif
}

static int fake_install(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	ARG_UNUSED(dev);
	install_calls++;
	installed = *cfg;
	return install_result;
}

static int fake_setup(const struct device *dev, uint8_t options)
{
	ARG_UNUSED(dev);
	setup_calls++;
	setup_options = options;
	return setup_result;
}

static int fake_feed(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	zassert_equal(channel_id, 0);
	feed_calls++;
	return 0;
}

static DEVICE_API(wdt, fake_api) = {
	.install_timeout = fake_install,
	.setup = fake_setup,
	.feed = fake_feed,
};

DEVICE_DT_DEFINE(DT_CHOSEN(zephyr_watchdog), fake_init, NULL, NULL, NULL,
		 POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_api);

static entelea_clock_t clock;

#ifdef TEST_WDT_HW_NOT_READY
ZTEST(wdt_hardware, test_not_ready_blocks_start)
{
	zassert_equal(entelea_platform_clock_init(&clock), ENTELEA_OK);
	zassert_equal(entelea_wdt_bind_clock(&clock), ENTELEA_OK);
	zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
	zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
	zassert_equal(install_calls, 0);
	zassert_equal(setup_calls, 0);
	zassert_equal(feed_calls, 0);
}
#else
static int boot_steps_seen;
static int boot_done_seen;
static uint8_t last_boot_step;

static void on_boot_step(const entelea_event_t *evt, void *user_data)
{
	const entelea_boot_step_evt_t *step = evt->data;

	ARG_UNUSED(user_data);
	boot_steps_seen++;
	last_boot_step = step->idx;
}

static void on_boot_done(const entelea_event_t *evt, void *user_data)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user_data);
	boot_done_seen++;
}

static void boot_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
}

K_THREAD_STACK_DEFINE(boot_stack, 8192);
static struct k_thread boot_thread;

ZTEST(wdt_hardware, test_real_task_wdt_backend_errors_and_recovery)
{
	zassert_equal(entelea_platform_clock_init(&clock), ENTELEA_OK);
	zassert_equal(entelea_wdt_bind_clock(&clock), ENTELEA_OK);

	static const int errors[] = {-ENOTSUP, -EIO, -EBUSY};
	static const entelea_out_channel_t probe = {
		.uid = "boot_probe",
		.kind = ENTELEA_CH_GPIO,
		.poweron = {.b = true},
		.linkloss = {.b = false},
		.fault = {.b = false},
	};
	entelea_ch_state_t state;
	entelea_out_value_t value;

	install_result = -EINVAL;
	zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
	zassert_equal(install_calls, 1);
	zassert_equal(setup_calls, 0);
	zassert_equal(feed_calls, 0);

	install_result = 0;
	for (int i = 0; i < CONFIG_TASK_WDT_CHANNELS + 2; i++) {
		setup_result = errors[i % ARRAY_SIZE(errors)];
		zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
		zassert_equal(setup_calls, i + 1);
		zassert_equal(feed_calls, 0, "Failed hardware setup must not be fed");
	}
	zassert_equal(installed.flags, WDT_FLAG_RESET_SOC);
	zassert_equal(installed.window.min, 0);
	zassert_equal(installed.window.max,
		      CONFIG_TASK_WDT_MIN_TIMEOUT + CONFIG_TASK_WDT_HW_FALLBACK_DELAY);
	zassert_is_null(installed.callback);
	zassert_equal(setup_options, 0);
	zassert_equal(install_calls, 2, "Retry must reuse the installed hardware timeout");

	/* ---- boot 失败集成（放末段：system_fail 置 halted 为一次性，
	 * 其后不得再有 wdt_start——缺陷 4 语义与测试顺序的共同要求） ---- */
	zassert_equal(entelea_safety_register_channel(&probe), ENTELEA_OK);
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, on_boot_step, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_DONE, on_boot_done, NULL),
		      ENTELEA_OK);
	setup_result = -EIO;
	k_tid_t tid = k_thread_create(&boot_thread, boot_stack,
				      K_THREAD_STACK_SIZEOF(boot_stack),
				      boot_entry, NULL, NULL, NULL, 0, 0, K_NO_WAIT);
	int join_result = k_thread_join(tid, K_MSEC(100));

	k_thread_abort(tid);
	zassert_equal(join_result, -EAGAIN, "Failed boot must remain halted");
	zassert_equal(boot_steps_seen, 3, "No later initialization step may run");
	zassert_equal(last_boot_step, 2);
	zassert_equal(boot_done_seen, 0);
	zassert_equal(entelea_safety_test_fail_reason(), ENTELEA_FAIL_BOOT(2));
	zassert_equal(entelea_safety_channel_state(probe.uid, &state), ENTELEA_OK);
	zassert_equal(state, ENTELEA_ST_SAFE_FAULT);
	zassert_equal(entelea_safety_readback(probe.uid, &value), ENTELEA_OK);
	zassert_false(value.b, "Failed boot must not retain the power-on output");
	/* boot 线程内 system_fail 已置全局停机（halted）：此后巡检不喂——
	 * 停机语义不再依赖调用线程挂起（缺陷 4）。 */
	zassert_false(entelea_wdt_test_patrol_tick());
	entelea_safety_test_reset();
}
#endif

ZTEST_SUITE(wdt_hardware, NULL, NULL, NULL, NULL, NULL);
