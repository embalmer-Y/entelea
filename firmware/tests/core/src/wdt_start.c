// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <ts/core.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/ztest.h>

static int init_result;
static int init_calls;
static int add_result;
static int add_calls;

int __wrap_task_wdt_init(const struct device *hw)
{
	ARG_UNUSED(hw);
	init_calls++;
	return init_result;
}

int __wrap_task_wdt_add(uint32_t reload_period, task_wdt_callback_t callback, void *user_data)
{
	ARG_UNUSED(reload_period);
	ARG_UNUSED(callback);
	ARG_UNUSED(user_data);
	add_calls++;
	return add_result;
}

ZTEST(wdt_start_failures, test_failed_start_never_reports_ready)
{
#ifdef TEST_WDT_REQUIRED_MISSING
	zassert_equal(ts_wdt_start(), TS_E_IO);
	zassert_equal(ts_wdt_start(), TS_E_IO);
	zassert_equal(init_calls, 0);
	zassert_equal(add_calls, 0);
#else
	init_result = -ENODEV;
	zassert_equal(ts_wdt_start(), TS_E_IO);
	zassert_equal(add_calls, 0, "Failed init must not allocate a channel");

	init_result = 0;
	add_result = -ENOMEM;
	zassert_equal(ts_wdt_start(), TS_E_NOMEM);
	zassert_equal(add_calls, 1);

	add_result = -EINVAL;
	zassert_equal(ts_wdt_start(), TS_E_IO);
	zassert_equal(add_calls, 2, "Failed start must not set the started flag");
	zassert_equal(init_calls, 2, "Retry must not reinitialize a running task watchdog");
#endif
}

ZTEST_SUITE(wdt_start_failures, NULL, NULL, NULL, NULL, NULL);
