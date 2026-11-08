/* SPDX-License-Identifier: Apache-2.0 */
/* R02 WDT 启动失败负测（迁自旧 firmware/tests/core/src/wdt_start.c）：
 * 后端失败不得报告成功、不分配通道、不置 started、重试不重复初始化。 */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>

static uint64_t virt_now;

static entelea_result_t virt_read_ms(void *user, uint64_t *out)
{
	ARG_UNUSED(user);
	*out = virt_now;
	return ENTELEA_OK;
}

static const entelea_clock_t virt_clock = {
	.read_ms = virt_read_ms,
	.user = NULL,
	.clock_id = 0x570D,
	.last_ms = 0,
	.sampled = false,
};

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

int __wrap_task_wdt_add(uint32_t reload_period, task_wdt_callback_t callback,
			void *user_data)
{
	ARG_UNUSED(reload_period);
	ARG_UNUSED(callback);
	ARG_UNUSED(user_data);
	add_calls++;
	return add_result;
}

static int feed_calls;

void __wrap_task_wdt_feed(int channel_id)
{
	ARG_UNUSED(channel_id);
	feed_calls++;
}

ZTEST(wdt_start_failures, test_failed_start_never_reports_ready)
{
	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 0;
#ifdef TEST_WDT_REQUIRED_MISSING
	/* 必需硬件看门狗未选定：task_wdt 面之前显式失败（缺必需设备负测）。 */
	zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
	zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
	zassert_equal(init_calls, 0);
	zassert_equal(add_calls, 0);
#else
	init_result = -ENODEV;
	zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
	zassert_equal(add_calls, 0, "Failed init must not allocate a channel");

	init_result = 0;
	add_result = -ENOMEM;
	zassert_equal(entelea_wdt_start(), ENTELEA_E_NOMEM);
	zassert_equal(add_calls, 1);

	add_result = -EINVAL;
	zassert_equal(entelea_wdt_start(), ENTELEA_E_IO);
	zassert_equal(add_calls, 2, "Failed start must not set the started flag");
	zassert_equal(init_calls, 2,
		      "Retry must not reinitialize a running task watchdog");
#endif
}

/* ---- 全局停机（缺陷 4）：halted 后任何线程视角巡检都不再喂狗 ----
 * （仅默认变体：REQUIRED_MISSING 变体 start 本就 E_IO，无通道可喂） */
#ifndef TEST_WDT_REQUIRED_MISSING
ZTEST(wdt_start_failures, test_halt_stops_patrol_from_any_context)
{
	entelea_wdt_test_reset();
	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 0;
	init_result = 0;
	add_result = 0;
	zassert_equal(entelea_wdt_start(), ENTELEA_OK);
	zassert_true(entelea_wdt_test_patrol_tick(), "halt 前巡检体应执行");
	/* 正向控制（三轮 review 发现 6）：wrapper 确实被链接且真实路径
	 * 经过它——零计数不得作为“零喂狗”的证据。 */
	zassert_true(feed_calls > 0, "positive control: patrol must feed task_wdt");
	entelea_wdt_halt();
	int snap = feed_calls;

	zassert_false(entelea_wdt_test_patrol_tick(), "halt 后巡检体不得执行");
	k_sleep(K_MSEC(200)); /* 已排程的后台巡检也不得喂 */
	zassert_equal(feed_calls, snap, "halted: no further task_wdt feeds");
	zassert_equal(entelea_wdt_start(), ENTELEA_E_STATE);
}

ZTEST(wdt_start_failures, test_system_fail_halts_globally)
{
	entelea_wdt_test_reset();
	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 0;
	init_result = 0;
	add_result = 0;
	zassert_equal(entelea_wdt_start(), ENTELEA_OK);
	/* 测试线程（非巡检上下文）触发 system_fail：测试构建置态后返回，
	 * 但停机语义必须是全局的——巡检立即停喂（缺陷 4 负测）。 */
	entelea_safety_system_fail(ENTELEA_FAIL_WDT(0));
	int snap = feed_calls;

	k_sleep(K_MSEC(200));
	zassert_equal(feed_calls, snap, "system_fail 后任何路径不得再喂狗");
	zassert_false(entelea_wdt_test_patrol_tick());
}
#endif /* !TEST_WDT_REQUIRED_MISSING */

ZTEST_SUITE(wdt_start_failures, NULL, NULL, NULL, NULL, NULL);
