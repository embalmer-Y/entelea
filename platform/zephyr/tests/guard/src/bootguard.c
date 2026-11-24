/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/guard.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"

static uint8_t retained_data[8];
static uint32_t reset_cause;
static bool retained_ready;
static bool fail_read;
static bool fail_write;
static bool fail_get_cause;
static bool fail_clear_cause;

static ssize_t
retained_size(const struct device *dev)
{
	ARG_UNUSED(dev);
	return sizeof(retained_data);
}

static int
retained_read(const struct device *dev, off_t offset, uint8_t *buffer,
	      size_t size)
{
	ARG_UNUSED(dev);
	if (fail_read) {
		return -EIO;
	}
	memcpy(buffer, retained_data + offset, size);
	return 0;
}

static int
retained_write(const struct device *dev, off_t offset, const uint8_t *buffer,
	       size_t size)
{
	ARG_UNUSED(dev);
	if (fail_write) {
		return -EIO;
	}
	memcpy(retained_data + offset, buffer, size);
	return 0;
}

static int
retained_clear(const struct device *dev)
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

DEVICE_DT_DEFINE(DT_NODELABEL(guardretained), NULL, NULL, NULL, NULL,
		 POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		 &retained_api);

int
__wrap_z_impl_hwinfo_get_reset_cause(uint32_t *cause)
{
	if (fail_get_cause) {
		return -EIO;
	}
	*cause = reset_cause;
	return 0;
}

int
__wrap_z_impl_hwinfo_clear_reset_cause(void)
{
	return fail_clear_cause ? -EIO : 0;
}

bool
__real_z_impl_device_is_ready(const struct device *dev);

bool
__wrap_z_impl_device_is_ready(const struct device *dev)
{
	if (dev == DEVICE_DT_GET(DT_NODELABEL(guardretained))) {
		return retained_ready && __real_z_impl_device_is_ready(dev);
	}
	return __real_z_impl_device_is_ready(dev);
}

static void
reset_fixture(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(retained_data, 0, sizeof(retained_data));
	reset_cause = RESET_POR;
	retained_ready = true;
	fail_read = false;
	fail_write = false;
	fail_get_cause = false;
	fail_clear_cause = false;
	entelea_guard_test_reset();
	entelea_platform_guard_test_reset();
}

ZTEST_SUITE(platform_guard_bootguard, NULL, NULL, reset_fixture, NULL, NULL);

ZTEST(platform_guard_bootguard, test_por_then_three_abnormal_resets)
{
	zassert_false(entelea_platform_guard_bootguard());
	reset_cause = RESET_WATCHDOG;
	zassert_false(entelea_platform_guard_bootguard());
	zassert_false(entelea_platform_guard_bootguard());
	zassert_true(entelea_platform_guard_bootguard());
	zassert_true(entelea_platform_guard_safe_halt());
}

ZTEST(platform_guard_bootguard, test_por_clears_valid_history)
{
	zassert_false(entelea_platform_guard_bootguard());
	reset_cause = RESET_WATCHDOG;
	zassert_false(entelea_platform_guard_bootguard());
	zassert_false(entelea_platform_guard_bootguard());
	reset_cause = RESET_POR;
	zassert_false(entelea_platform_guard_bootguard());
	reset_cause = RESET_WATCHDOG;
	zassert_false(entelea_platform_guard_bootguard());
	zassert_false(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard, test_abnormal_reset_with_unknown_history_halts)
{
	reset_cause = RESET_WATCHDOG;
	zassert_true(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard, test_por_with_unready_retained_device_halts)
{
	retained_ready = false;
	zassert_true(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard, test_por_with_retained_read_failure_halts)
{
	fail_read = true;
	zassert_true(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard,
      test_read_failure_latches_following_abnormal_boot)
{
	fail_read = true;
	zassert_true(entelea_platform_guard_bootguard());
	entelea_platform_guard_test_reset();
	fail_read = false;
	reset_cause = RESET_WATCHDOG;
	zassert_true(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard, test_por_with_retained_write_failure_halts)
{
	fail_write = true;
	zassert_true(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard, test_unknown_reset_cause_halts)
{
	zassert_false(entelea_platform_guard_bootguard());
	fail_get_cause = true;
	zassert_true(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard, test_failure_to_clear_reset_cause_halts)
{
	fail_clear_cause = true;
	zassert_true(entelea_platform_guard_bootguard());
}

ZTEST(platform_guard_bootguard, test_mixed_por_and_brownout_is_abnormal)
{
	zassert_false(entelea_platform_guard_bootguard());
	reset_cause = RESET_POR | RESET_BROWNOUT;
	zassert_false(entelea_platform_guard_bootguard());
	zassert_false(entelea_platform_guard_bootguard());
	zassert_true(entelea_platform_guard_bootguard());
}

/* ---- 复位风暴端到端（owner 2026-11-24）：真实守卫 + 真实启动表 ----
 * 业务步骤 storage_init 每次启动都失败（介质读不出）。守卫排在首步，
 * 连续三次异常复位之后的那次启动只建立安全态，不再复位。
 * 执行器经平台钩子用 printk 报告；这里链接时包装 printk 捕获控制台。 */
static char console_text[2048];
static size_t console_len;

void
__wrap_printk(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (console_len < sizeof(console_text) - 1) {
		int n = vsnprintk(console_text + console_len,
				  sizeof(console_text) - console_len, fmt, ap);

		if (n > 0) {
			console_len += MIN((size_t) n, sizeof(console_text) -
							       1 - console_len);
		}
	}
	vprintk(fmt, ap);
	va_end(ap);
}

static bool
console_has(const char *needle)
{
	return strstr(console_text, needle) != NULL;
}

static entelea_store_ops_t storm_base;

static entelea_result_t
unusable_read(entelea_store_part_t part, uint32_t off, void *buf, uint32_t len)
{
	ARG_UNUSED(part);
	ARG_UNUSED(off);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return ENTELEA_E_IO;
}

static entelea_clock_t boot_clock;
static uint32_t boot_fail_reason;
static bool boot_returned;

static const char failed_line[] =
	"[boot] step 5 \"storage_init\" failed: -8 -> fail-safe";
static const char skipped_line[] =
	"[boot] step 5 \"storage_init\" not run: SAFE-HALT";

K_THREAD_STACK_DEFINE(boot_stack, 8192);
static struct k_thread boot_thread;

static void
boot_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
}

/* 一次启动，按装配顺序（同 minimal 镜像的 main）。RAM 里的状态在复位时
 * 丢失：安全态、看门狗、守卫的闩与端口都在每次启动时重建；留存寄存器
 * 与复位原因由调用者给定。结果：boot_returned = 执行器返回了（没有步骤
 * 失败），boot_fail_reason = 本次启动是否走过失效保护。 */
static void
boot_once(uint32_t cause)
{
	reset_cause = cause;
	entelea_safety_test_reset();
	entelea_wdt_test_reset();
	entelea_guard_test_reset();
	entelea_platform_guard_test_reset();
	console_len = 0;
	console_text[0] = '\0';
	zassert_ok(entelea_platform_clock_init(&boot_clock));
	zassert_ok(entelea_evt_bind_clock(&boot_clock));
	zassert_ok(entelea_wdt_bind_clock(&boot_clock));
	zassert_ok(entelea_safety_bind_clock(&boot_clock));

	k_tid_t tid = k_thread_create(
		&boot_thread, boot_stack, K_THREAD_STACK_SIZEOF(boot_stack),
		boot_entry, NULL, NULL, NULL, 0, 0, K_NO_WAIT);
	int joined = k_thread_join(tid, K_MSEC(300));

	boot_fail_reason = entelea_safety_test_fail_reason();
	boot_returned = joined == 0;
	if (joined != 0) {
		/* 失败的启动停在失效保护里不返回：真机上这里被复位取代。 */
		k_thread_abort(tid);
	}
}

/* 失败的启动：不返回，走过失效保护，控制台点出失败的步骤。 */
static void
expect_failed_boot(int boot)
{
	zassert_false(boot_returned, "boot %d", boot);
	zassert_equal(boot_fail_reason, ENTELEA_FAIL_BOOT(5));
	zassert_true(console_has(failed_line), "boot %d console: %s", boot,
		     console_text);
	zassert_false(console_has(skipped_line));
	zassert_false(entelea_platform_guard_safe_halt());
}

/* SAFE-HALT 的启动：执行器返回，没有走失效保护，业务步骤没有运行。 */
static void
expect_halted_boot(void)
{
	zassert_true(boot_returned, "SAFE-HALT 启动返回");
	zassert_equal(boot_fail_reason, 0, "不走失效保护");
	zassert_true(console_has(skipped_line), "console: %s", console_text);
	zassert_false(console_has(failed_line));
	zassert_true(entelea_platform_guard_safe_halt());
	zassert_equal(entelea_storage_audit_append(
			      &(entelea_storage_audit_entry_t) { 0 }, 0),
		      ENTELEA_E_STATE, "存储没有初始化：业务确实没有启动");
}

ZTEST(platform_guard_bootguard,
      test_failing_boot_step_ends_in_safe_halt_not_a_reset_loop)
{
	entelea_store_ops_t ops = entelea_store_backend;

	storm_base = entelea_store_backend;
	ops.read = unusable_read;
	zassert_equal(entelea_storage_mount(&ops), ENTELEA_OK);

	/* 上电后的第 1 到第 3 次启动：业务步骤失败，走失效保护被复位。 */
	boot_once(RESET_POR);
	expect_failed_boot(1);
	boot_once(RESET_WATCHDOG);
	expect_failed_boot(2);
	boot_once(RESET_WATCHDOG);
	expect_failed_boot(3);

	/* 第 4 次：守卫判定 SAFE-HALT，只建立安全态，业务步骤不运行，
	 * 执行器返回——下一次复位不会再来。 */
	boot_once(RESET_WATCHDOG);
	expect_halted_boot();
	/* 故障还在时再来一次复位：仍是 SAFE-HALT。 */
	boot_once(RESET_WATCHDOG);
	expect_halted_boot();

	/* 断电重启（纯上电复位）且介质恢复：计数清零，完整启动。 */
	zassert_equal(entelea_storage_mount(&storm_base), ENTELEA_OK);
	boot_once(RESET_POR);
	zassert_true(boot_returned);
	zassert_equal(boot_fail_reason, 0);
	zassert_false(entelea_platform_guard_safe_halt());
	zassert_false(console_has(skipped_line), "console: %s", console_text);
	zassert_equal(entelea_storage_audit_append(
			      &(entelea_storage_audit_entry_t) { 0 }, 0),
		      ENTELEA_OK, "存储已初始化");
	entelea_wdt_test_reset();
}
