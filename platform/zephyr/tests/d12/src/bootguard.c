/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/ztest.h>
#include <entelea/d12.h>
#include "boot_steps.h"

static uint8_t retained_data[8];
static uint32_t reset_cause;
static bool retained_ready;
static bool fail_read;
static bool fail_write;
static bool fail_get_cause;
static bool fail_clear_cause;

static ssize_t retained_size(const struct device *dev)
{
	ARG_UNUSED(dev);
	return sizeof(retained_data);
}

static int retained_read(const struct device *dev, off_t offset,
			 uint8_t *buffer, size_t size)
{
	ARG_UNUSED(dev);
	if (fail_read) {
		return -EIO;
	}
	memcpy(buffer, retained_data + offset, size);
	return 0;
}

static int retained_write(const struct device *dev, off_t offset,
			  const uint8_t *buffer, size_t size)
{
	ARG_UNUSED(dev);
	if (fail_write) {
		return -EIO;
	}
	memcpy(retained_data + offset, buffer, size);
	return 0;
}

static int retained_clear(const struct device *dev)
{
	ARG_UNUSED(dev);
	memset(retained_data, 0, sizeof(retained_data));
	return 0;
}

static DEVICE_API(retained_mem, retained_api) = {
	.size = retained_size,
	.read = retained_read,
	.write = retained_write,
	.clear = retained_clear,
};

DEVICE_DT_DEFINE(DT_NODELABEL(d12retained), NULL, NULL, NULL, NULL,
		 POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &retained_api);

int __wrap_z_impl_hwinfo_get_reset_cause(uint32_t *cause)
{
	if (fail_get_cause) {
		return -EIO;
	}
	*cause = reset_cause;
	return 0;
}

int __wrap_z_impl_hwinfo_clear_reset_cause(void)
{
	return fail_clear_cause ? -EIO : 0;
}

bool __real_z_impl_device_is_ready(const struct device *dev);

bool __wrap_z_impl_device_is_ready(const struct device *dev)
{
	if (dev == DEVICE_DT_GET(DT_NODELABEL(d12retained))) {
		return retained_ready && __real_z_impl_device_is_ready(dev);
	}
	return __real_z_impl_device_is_ready(dev);
}

static void reset_fixture(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(retained_data, 0, sizeof(retained_data));
	reset_cause = RESET_POR;
	retained_ready = true;
	fail_read = false;
	fail_write = false;
	fail_get_cause = false;
	fail_clear_cause = false;
	entelea_d12_test_reset();
	entelea_platform_d12_test_reset();
}

ZTEST_SUITE(platform_d12_bootguard, NULL, NULL, reset_fixture, NULL, NULL);

ZTEST(platform_d12_bootguard, test_por_then_three_abnormal_resets)
{
	zassert_false(entelea_platform_d12_bootguard());
	reset_cause = RESET_WATCHDOG;
	zassert_false(entelea_platform_d12_bootguard());
	zassert_false(entelea_platform_d12_bootguard());
	zassert_true(entelea_platform_d12_bootguard());
	zassert_true(entelea_platform_d12_safe_halt());
}

ZTEST(platform_d12_bootguard, test_por_clears_valid_history)
{
	zassert_false(entelea_platform_d12_bootguard());
	reset_cause = RESET_WATCHDOG;
	zassert_false(entelea_platform_d12_bootguard());
	zassert_false(entelea_platform_d12_bootguard());
	reset_cause = RESET_POR;
	zassert_false(entelea_platform_d12_bootguard());
	reset_cause = RESET_WATCHDOG;
	zassert_false(entelea_platform_d12_bootguard());
	zassert_false(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_abnormal_reset_with_unknown_history_halts)
{
	reset_cause = RESET_WATCHDOG;
	zassert_true(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_por_with_unready_retained_device_halts)
{
	retained_ready = false;
	zassert_true(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_por_with_retained_read_failure_halts)
{
	fail_read = true;
	zassert_true(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_read_failure_latches_following_abnormal_boot)
{
	fail_read = true;
	zassert_true(entelea_platform_d12_bootguard());
	entelea_platform_d12_test_reset();
	fail_read = false;
	reset_cause = RESET_WATCHDOG;
	zassert_true(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_por_with_retained_write_failure_halts)
{
	fail_write = true;
	zassert_true(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_unknown_reset_cause_halts)
{
	zassert_false(entelea_platform_d12_bootguard());
	fail_get_cause = true;
	zassert_true(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_failure_to_clear_reset_cause_halts)
{
	fail_clear_cause = true;
	zassert_true(entelea_platform_d12_bootguard());
}

ZTEST(platform_d12_bootguard, test_mixed_por_and_brownout_is_abnormal)
{
	zassert_false(entelea_platform_d12_bootguard());
	reset_cause = RESET_POR | RESET_BROWNOUT;
	zassert_false(entelea_platform_d12_bootguard());
	zassert_false(entelea_platform_d12_bootguard());
	zassert_true(entelea_platform_d12_bootguard());
}
