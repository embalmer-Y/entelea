// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <ts/core.h>
#include <ts/safety.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/ztest.h>

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

DEVICE_DT_DEFINE(DT_CHOSEN(zephyr_watchdog), fake_init, NULL, NULL, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_api);

#ifdef TEST_WDT_HW_NOT_READY
ZTEST(wdt_hardware, test_not_ready_blocks_start)
{
	zassert_equal(ts_wdt_start(), TS_E_IO);
	zassert_equal(ts_wdt_start(), TS_E_IO);
	zassert_equal(install_calls, 0);
	zassert_equal(setup_calls, 0);
	zassert_equal(feed_calls, 0);
}
#else
static int boot_steps_seen;
static int boot_done_seen;
static uint8_t last_boot_step;

static void on_boot_step(const ts_evt_t *evt, void *user_data)
{
	const ts_boot_step_evt_t *step = evt->data;

	ARG_UNUSED(user_data);
	boot_steps_seen++;
	last_boot_step = step->idx;
}

static void on_boot_done(const ts_evt_t *evt, void *user_data)
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
	ts_core_boot();
}

K_THREAD_STACK_DEFINE(boot_stack, 8192);
static struct k_thread boot_thread;

ZTEST(wdt_hardware, test_real_task_wdt_backend_errors_and_recovery)
{
	static const int errors[] = {-ENOTSUP, -EIO, -EBUSY};
	static const ts_out_ch_t probe = {
		.uid = "boot_probe",
		.kind = TS_CH_GPIO,
		.poweron = {.b = true},
		.linkloss = {.b = false},
		.fault = {.b = false},
	};
	ts_ch_state_t state;
	ts_out_value_t value;

	install_result = -EINVAL;
	zassert_equal(ts_wdt_start(), TS_E_IO);
	zassert_equal(install_calls, 1);
	zassert_equal(setup_calls, 0);
	zassert_equal(feed_calls, 0);

	install_result = 0;
	for (int i = 0; i < CONFIG_TASK_WDT_CHANNELS + 2; i++) {
		setup_result = errors[i % ARRAY_SIZE(errors)];
		zassert_equal(ts_wdt_start(), TS_E_IO);
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

	zassert_equal(ts_safety_register_channel(&probe), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_STEP, on_boot_step, NULL), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_DONE, on_boot_done, NULL), TS_OK);
	setup_result = -EIO;
	k_tid_t tid = k_thread_create(&boot_thread, boot_stack, K_THREAD_STACK_SIZEOF(boot_stack),
				      boot_entry, NULL, NULL, NULL, 0, 0, K_NO_WAIT);
	int join_result = k_thread_join(tid, K_MSEC(100));

	k_thread_abort(tid);
	zassert_equal(join_result, -EAGAIN, "Failed boot must remain halted");
	zassert_equal(boot_steps_seen, 3, "No later initialization step may run");
	zassert_equal(last_boot_step, 2);
	zassert_equal(boot_done_seen, 0);
	zassert_equal(ts_safety_test_fail_reason(), TS_FAIL_BOOT(2));
	zassert_equal(ts_safety_channel_state(probe.uid, &state), TS_OK);
	zassert_equal(state, TS_ST_SAFE_FAULT);
	zassert_equal(ts_safety_readback(probe.uid, &value), TS_OK);
	zassert_false(value.b, "Failed boot must not retain the power-on output");
	ts_safety_test_reset();

	setup_result = 0;
	zassert_equal(ts_wdt_start(), TS_OK);
	zassert_true(feed_calls > 0, "Successful hardware start must be fed");
	zassert_equal(ts_wdt_start(), TS_E_STATE);

	int extra[CONFIG_TASK_WDT_CHANNELS - 1];
	int setup_before = setup_calls;

	for (int i = 0; i < ARRAY_SIZE(extra); i++) {
		extra[i] = task_wdt_add(CONFIG_TS_WDT_CHANNEL_MS, NULL, NULL);
		zassert_equal(extra[i], i + 1, "Failed setup must not leak channels");
	}
	zassert_equal(task_wdt_add(CONFIG_TS_WDT_CHANNEL_MS, NULL, NULL), -ENOMEM);
	zassert_equal(setup_calls, setup_before, "Hardware must start only once");
	for (int i = 0; i < ARRAY_SIZE(extra); i++) {
		zassert_equal(task_wdt_delete(extra[i]), 0);
	}
}
#endif

ZTEST_SUITE(wdt_hardware, NULL, NULL, NULL, NULL, NULL);
