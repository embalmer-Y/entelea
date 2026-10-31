/* SPDX-License-Identifier: Apache-2.0 */
/* framework.threads（V2 纵深批 M19a，DEC-55/Q-37 A）：真 wasm 多线程
 * sim 回归——宿主侧 spawn 编排全链。
 *   test_01 双线程 spawn/join/原子共享：threads=3 申报 → 并发 2 worker
 *           （共享线性内存原子累加）→ join 双返回值 + g_counter==3000
 *           → PASS 旗（gpio0）；
 *   test_02 未申报拒绝：不调 set_threads（threads_max<2）→ spawn 返回
 *           E_STATE → FAIL 旗（gpio1）——DEC-31 单线程语义 fail-closed；
 *   test_03 并发 cap：threads=2 → 第二并发 spawn E_RANGE → join 后槽
 *           复用重 spawn → 全量汇合 PASS 旗。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <ts/appmgr.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include "thread_app_wasm.h"

static const ts_out_ch_t ch0 = {
	.uid = "th0", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_out_ch_t ch1 = {
	.uid = "th1", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_hal_dev_desc_t dev0 = {.uid = "th0", .kind = TS_DEV_GPIO_OUT};
static const ts_hal_dev_desc_t dev1 = {.uid = "th1", .kind = TS_DEV_GPIO_OUT};
static bool hal_done;

static bool gpio_val(int inst)
{
	ts_out_value_t v;
	char uid[8] = {'t', 'h', '0'};

	uid[2] = (char)('0' + inst);
	if (ts_safety_readback(uid, &v) != TS_OK) {
		return false;
	}
	return v.b;
}

static bool wait_gpio(int inst, bool want)
{
	for (int i = 0; i < 300; i++) {
		if (gpio_val(inst) == want) {
			return true;
		}
		k_msleep(10);
	}
	return false;
}

static void suite_setup(void)
{
	ts_safety_test_reset();
	ts_appmgr_app_test_reset();
	if (!hal_done) { /* hal 注册表进程级共享——一次即可 */
		zassert_equal(ts_hal_register_dev(&dev0), TS_OK);
		zassert_equal(ts_hal_register_dev(&dev1), TS_OK);
		hal_done = true;
	}
	zassert_equal(ts_safety_register_channel(&ch0), TS_OK);
	zassert_equal(ts_safety_register_channel(&ch1), TS_OK);
	ts_safety_set_link(true);
}

ZTEST(framework_threads, test_01_spawn_join_atomic)
{
	suite_setup();
	ts_appmgr_app_set_threads(1, 3);
	zassert_equal(ts_appmgr_app_start(1, thread_app_wasm, thread_app_wasm_len,
					  "gpio:write:0-1"), TS_OK);
	zassert_true(wait_gpio(0, true),
		     "双 worker 原子累加 3000 + join 返回值校验 → PASS 旗");
	zassert_false(gpio_val(1), "FAIL 旗未起");
	struct ts_app_rt_stats st;

	ts_appmgr_app_stats(&st);
	zassert_equal(st.health_fails, 0, "健康无失败（worker 异常会计入）");
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

ZTEST(framework_threads, test_02_undeclared_reject)
{
	suite_setup();
	/* 不调 set_threads（threads_max=0<2）——spawn 必须 E_STATE 拒绝 */
	zassert_equal(ts_appmgr_app_start(1, thread_app_wasm, thread_app_wasm_len,
					  "gpio:write:0-1"), TS_OK);
	zassert_true(wait_gpio(1, true), "未申报 threads → spawn 拒绝 → FAIL 旗");
	zassert_false(gpio_val(0), "PASS 旗未起");
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

ZTEST(framework_threads, test_03_concurrency_cap)
{
	suite_setup();
	ts_appmgr_app_set_threads(1, 2); /* 并发 worker 上限 = 1 */
	zassert_equal(ts_appmgr_app_start(1, thread_app_wasm, thread_app_wasm_len,
					  "gpio:write:0-1"), TS_OK);
	zassert_true(wait_gpio(0, true),
		     "cap 拒绝 → join 后槽复用重 spawn → 全量汇合 PASS 旗");
	zassert_false(gpio_val(1), "FAIL 旗未起");
	zassert_equal(ts_appmgr_app_stop(), TS_OK);
}

ZTEST_SUITE(framework_threads, NULL, NULL, NULL, NULL, NULL);
