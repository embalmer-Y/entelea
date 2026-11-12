/* SPDX-License-Identifier: Apache-2.0 */
/* r16 D12 闸 ztest：fail-closed、频度 6/60s、E_BUSY 冷却 60s、风暴 3 闩
 * +外部 clear、异常复位冷却窗、观测计数；RF1 G5：t=0 槽、时钟回退、
 * 时钟失明期冷却不丢、重绑时钟不清风暴闩；WP4a：UTC 日额 200（持久预留）、
 * 掉电重启不重置、墙钟/持久/互斥端口失败拒绝、写入预算与多线程并发限额。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/atomic.h>
#include <entelea/guard.h>
#include <entelea/time.h>
#include "guard_ports.h"

#define DAY0 20000ULL /* UTC 日序号；测试墙钟起点 */
#define DAY_SECS 86400ULL

static uint64_t fake_now;
static bool fake_fail;
static entelea_clock_t clk;
static uint64_t fake_utc_s = DAY0 * DAY_SECS;
static bool wall_fail;
static entelea_lock_t lock;
static uint8_t rec[ENTELEA_GUARD_REC_LEN];
static bool rec_present;
static bool load_fail;
static bool save_fail;
static int save_count;

static entelea_result_t
fake_read(void *user, uint64_t *out)
{
	if (fake_fail) {
		return ENTELEA_E_IO;
	}
	*out = *(uint64_t *) user;
	return ENTELEA_OK;
}

static entelea_result_t
fake_wall_read(void *user, uint64_t *out)
{
	if (wall_fail) {
		return ENTELEA_E_STATE;
	}
	*out = *(uint64_t *) user;
	return ENTELEA_OK;
}

static entelea_result_t
fake_load(void *user, uint8_t out[ENTELEA_GUARD_REC_LEN])
{
	(void) user;
	if (load_fail) {
		return ENTELEA_E_IO;
	}
	if (!rec_present) {
		return ENTELEA_E_NOTFOUND;
	}
	memcpy(out, rec, ENTELEA_GUARD_REC_LEN);
	return ENTELEA_OK;
}

static entelea_result_t
fake_save(void *user, const uint8_t in[ENTELEA_GUARD_REC_LEN])
{
	(void) user;
	if (save_fail) {
		return ENTELEA_E_IO;
	}
	memcpy(rec, in, ENTELEA_GUARD_REC_LEN);
	rec_present = true;
	save_count++;
	return ENTELEA_OK;
}

static entelea_wallclock_t wall = {
	.read_utc_s = fake_wall_read,
	.user = &fake_utc_s,
};

static entelea_guard_store_t store = {
	.load = fake_load,
	.save = fake_save,
	.user = NULL,
};

/* 重绑全部端口（不清持久记录）：模拟掉电重启后的启动装配。 */
static void
reboot(void)
{
	entelea_guard_test_reset();
	zassert_ok(entelea_clock_init(&clk, fake_read, &fake_now, 1));
	zassert_ok(entelea_guard_bind_clock(&clk));
	zassert_ok(entelea_guard_bind_lock(&lock));
	zassert_ok(entelea_guard_bind_wallclock(&wall));
	zassert_ok(entelea_guard_bind_store(&store));
}

/* 新设备：清持久记录，墙钟置于 DAY0 当日。 */
static void
bind(uint64_t t0)
{
	fake_now = t0;
	fake_fail = false;
	wall_fail = false;
	load_fail = false;
	save_fail = false;
	rec_present = false;
	save_count = 0;
	fake_utc_s = DAY0 * DAY_SECS;
	zassert_ok(entelea_platform_guard_lock_init(&lock));
	reboot();
}

/* 测试内独立编码的持久记录（格式见 guard.c），用于预置计数状态。 */
static void
seed(uint32_t day, uint16_t reserved)
{
	static const uint8_t magic[4] = { 'D', '1', '2', 'G' };

	memcpy(rec, magic, sizeof(magic));
	rec[4] = 1;
	rec[5] = 0;
	rec[6] = (uint8_t) (reserved & 0xFF);
	rec[7] = (uint8_t) (reserved >> 8);
	rec[8] = (uint8_t) (day & 0xFF);
	rec[9] = (uint8_t) ((day >> 8) & 0xFF);
	rec[10] = (uint8_t) ((day >> 16) & 0xFF);
	rec[11] = (uint8_t) ((day >> 24) & 0xFF);
	rec_present = true;
}

ZTEST_SUITE(framework_guard, NULL, NULL, NULL, NULL, NULL);

ZTEST(framework_guard, test_fail_closed_without_clock)
{
	entelea_guard_test_reset();
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE);
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_STATE);
}

ZTEST(framework_guard, test_rate_window_and_lifetime)
{
	bind(1000);
	for (int i = 0; i < 6; i++) {
		zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
	}
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "第 7 次（60s 窗内）拒");
	fake_now += 61000; /* 出窗 */
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
	zassert_equal(entelea_guard_model_blocked(), 1);
}

ZTEST(framework_guard, test_busy_cooldown)
{
	bind(1000);
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
	entelea_guard_model_note(ENTELEA_E_BUSY);
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY, "冷却内拒");
	fake_now += 61000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK, "冷却出");
}

ZTEST(framework_guard, test_storm_latch_and_clear)
{
	bind(1000);
	entelea_guard_model_note(ENTELEA_E_IO);
	entelea_guard_model_note(ENTELEA_E_IO);
	zassert_false(entelea_guard_storm_latched(), "2 败未闩");
	entelea_guard_model_note(ENTELEA_E_IO);
	zassert_true(entelea_guard_storm_latched(), "3 连败闩");
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE, "闩内拒");
	entelea_guard_model_note(ENTELEA_OK); /* 闩内成功上报不自动解 */
	zassert_true(entelea_guard_storm_latched());
	entelea_guard_storm_clear();
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK, "外部释放");
}

ZTEST(framework_guard, test_boot_cooldown_covers_wasm)
{
	bind(1000);
	entelea_guard_boot_note(true); /* 异常复位 */
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_BUSY, "冷却内拒");
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY);
	fake_now += 61000;
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_OK);
	entelea_guard_boot_note(false); /* 正常复位无窗 */
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_OK);
}

/* RF1 G5（审计 #11）：t=0 发生的调用必须占窗（旧实现 window==0 当空槽）。 */
ZTEST(framework_guard, test_rate_counts_calls_at_t_zero)
{
	bind(0);
	for (int i = 0; i < 6; i++) {
		zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
	}
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "t=0 的 6 次已占满 60s 窗");
	zassert_equal(entelea_guard_model_calls(), 6);
}

/* RF1 G5：时钟回退不得经无符号下溢放行（单调夹紧，窗/冷却不缩短）。 */
ZTEST(framework_guard, test_clock_regression_keeps_window)
{
	bind(100000);
	for (int i = 0; i < 6; i++) {
		zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
	}
	fake_now = 50000; /* 回退 */
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "回退不放行");
	fake_now = 159999;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY, "窗未出");
	fake_now = 160000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK, "出窗");
}

/* RF1 G5：异常复位上报时时钟失明——冷却挂起至首次好读起算，不丢。 */
ZTEST(framework_guard, test_boot_cooldown_survives_clock_fault)
{
	bind(1000);
	fake_fail = true;
	entelea_guard_boot_note(true);
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_STATE, "失明闭锁");
	fake_fail = false;
	fake_now = 5000;
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_BUSY, "冷却补立");
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY);
	fake_now = 5000 + 59999;
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_BUSY);
	fake_now = 5000 + 60000;
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_OK);
}

/* RF1 G5：E_BUSY 上报时时钟失明——429 冷却同样挂起不丢。 */
ZTEST(framework_guard, test_busy_cooldown_survives_clock_fault)
{
	bind(1000);
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
	fake_fail = true;
	entelea_guard_model_note(ENTELEA_E_BUSY);
	fake_fail = false;
	fake_now = 2000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "冷却补立");
	fake_now = 2000 + 60000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
}

ZTEST(framework_guard, test_boot_cooldown_does_not_wrap_at_clock_limit)
{
	bind(UINT64_MAX - 1000);
	entelea_guard_boot_note(true);
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_BUSY);
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY);
	fake_now = UINT64_MAX;
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_BUSY);
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY);
}

ZTEST(framework_guard, test_busy_cooldown_does_not_wrap_at_clock_limit)
{
	bind(UINT64_MAX - 1000);
	zassert_ok(entelea_guard_model_allow());
	entelea_guard_model_note(ENTELEA_E_BUSY);
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY);
	fake_now = UINT64_MAX;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY);
}

/* RF1 G5：重绑时钟不是风暴释放面（仅 storm_clear）。 */
ZTEST(framework_guard, test_storm_survives_clock_rebind)
{
	bind(1000);
	for (int i = 0; i < 3; i++) {
		entelea_guard_model_note(ENTELEA_E_IO);
	}
	zassert_true(entelea_guard_storm_latched());
	zassert_ok(entelea_guard_bind_clock(&clk));
	zassert_true(entelea_guard_storm_latched(), "重绑不清闩");
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE);
}

/* RF1 G5：跨复位风暴判定——POR 起第 3 次连续异常复位即 SAFE-HALT
 * （≥3，非 >3）；纯 POR 清零；计数饱和。 */
ZTEST(framework_guard, test_reset_guard_threshold_and_por)
{
	uint32_t c = 0;

	zassert_false(entelea_guard_reset_guard(false, true, &c));
	zassert_equal(c, 0);
	zassert_false(entelea_guard_reset_guard(true, true, &c));
	zassert_false(entelea_guard_reset_guard(true, true, &c));
	zassert_true(entelea_guard_reset_guard(true, true, &c),
		     "第 3 次即停");
	zassert_equal(c, ENTELEA_GUARD_RESET_STORM_MAX);
	zassert_true(entelea_guard_reset_guard(true, true, &c));
	zassert_equal(c, ENTELEA_GUARD_RESET_STORM_MAX, "饱和不回绕");
	zassert_false(entelea_guard_reset_guard(false, true, &c));
	zassert_equal(c, 0, "POR 出风暴窗");
}

/* RF1 G5：异常复位且留存历史不可知 → fail-closed；POR 时不可知
 * 不闭锁（POR 本身即出窗）。 */
ZTEST(framework_guard, test_reset_guard_unknown_history_fails_closed)
{
	uint32_t c = 0;

	zassert_true(entelea_guard_reset_guard(true, true, NULL));
	zassert_true(entelea_guard_reset_guard(true, false, &c));
	zassert_equal(c, ENTELEA_GUARD_RESET_STORM_MAX);
	c = 0xdeadbeef;
	zassert_false(entelea_guard_reset_guard(false, false, &c));
	zassert_equal(c, 0);
}

ZTEST(framework_guard, test_unbound_lock_refuses)
{
	bind(1000);
	entelea_guard_test_reset();
	zassert_ok(entelea_clock_init(&clk, fake_read, &fake_now, 1));
	zassert_ok(entelea_guard_bind_clock(&clk));
	zassert_ok(entelea_guard_bind_wallclock(&wall));
	zassert_ok(entelea_guard_bind_store(&store));
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE,
		      "未绑锁即拒");
	zassert_equal(entelea_guard_wasm_allow(), ENTELEA_E_STATE);
	zassert_equal(entelea_guard_model_calls(), 0);
	zassert_true(entelea_guard_storm_latched(), "未绑锁报闭锁");
}

ZTEST(framework_guard, test_wall_unknown_refuses)
{
	bind(1000);
	wall_fail = true;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE,
		      "墙钟未知拒");
	zassert_equal(save_count, 0, "拒绝不写持久");
	zassert_equal(entelea_guard_model_calls(), 0, "拒绝不计放行");
	wall_fail = false;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK,
		      "墙钟恢复后放行");
}

ZTEST(framework_guard, test_store_load_failure_refuses)
{
	bind(1000);
	load_fail = true;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE,
		      "读失败拒，不当作无记录");
	load_fail = false;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK, "读恢复后放行");
}

ZTEST(framework_guard, test_store_corrupt_record_refuses)
{
	bind(1000);
	seed(DAY0, 10);
	rec[0] = 'X'; /* 魔数损坏 */
	reboot();
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE, "魔数坏拒");

	seed(DAY0, 10);
	rec[4] = 2; /* 未知版本 */
	reboot();
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE, "版本坏拒");

	seed(DAY0, 201); /* 预留超过日额上限 */
	reboot();
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE, "越界拒");
}

ZTEST(framework_guard, test_store_save_failure_refuses_without_admission)
{
	bind(1000);
	save_fail = true;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_STATE,
		      "写失败不放行");
	zassert_equal(entelea_guard_model_calls(), 0);
	zassert_equal(save_count, 0);
	save_fail = false;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK, "写恢复后放行");
	zassert_equal(save_count, 1, "首次预留写入一次");
}

/* 掉电重启不重置日额：预留 190 → 10 次放行 → 用尽 → 重启仍用尽 → 次日恢复。 */
ZTEST(framework_guard, test_daily_cap_survives_reboot)
{
	bind(1000);
	seed(DAY0, 190);
	reboot();
	for (int i = 0; i < 10; i++) {
		fake_now += 60000; /* 逐次出 60s 窗，只受日额约束 */
		zassert_equal(entelea_guard_model_allow(), ENTELEA_OK,
			      "余量 10");
	}
	fake_now += 60000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "日额 200 用尽");
	reboot();
	fake_now += 60000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "重启不重置");
	fake_utc_s += DAY_SECS; /* 跨 UTC 日界 */
	zassert_equal(entelea_guard_model_allow(), ENTELEA_OK, "次日重新计数");
}

/* 每日 200 次按步长 10 预留，写入次数恰为 20（远低于磨损预算）。 */
ZTEST(framework_guard, test_daily_writes_stay_within_budget)
{
	bind(1000);
	for (int i = 0; i < 200; i++) {
		fake_now += 60000;
		zassert_equal(entelea_guard_model_allow(), ENTELEA_OK);
	}
	fake_now += 60000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "第 201 次拒");
	zassert_equal(save_count, 20, "步长 10 预留写入");
}

/* 墙钟回退不放宽：存储日更晚时沿用其上限。 */
ZTEST(framework_guard, test_utc_regression_does_not_loosen_daily_cap)
{
	bind(1000);
	seed(DAY0 + 1, 200);
	reboot();
	fake_now += 60000;
	zassert_equal(entelea_guard_model_allow(), ENTELEA_E_BUSY,
		      "回退不放行");
}

#define CONC_THREADS 3
#define CONC_ATTEMPTS 20

K_THREAD_STACK_ARRAY_DEFINE(conc_stack, CONC_THREADS, 2048);
static struct k_thread conc_thread[CONC_THREADS];
static struct k_sem conc_done;
static atomic_t conc_ok;

static void
conc_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (int i = 0; i < CONC_ATTEMPTS; i++) {
		if (entelea_guard_model_allow() == ENTELEA_OK) {
			atomic_inc(&conc_ok);
		}
	}
	k_sem_give(&conc_done);
}

/* 多线程并发（真实互斥端口）：当日余量 4，时间固定，放行必须恰为 4。 */
ZTEST(framework_guard, test_concurrent_admission_respects_daily_cap)
{
	bind(1000);
	seed(DAY0, 196);
	reboot();
	atomic_set(&conc_ok, 0);
	k_sem_init(&conc_done, 0, CONC_THREADS);
	for (int i = 0; i < CONC_THREADS; i++) {
		k_thread_create(&conc_thread[i], conc_stack[i],
				K_THREAD_STACK_SIZEOF(conc_stack[i]),
				conc_worker, NULL, NULL, NULL,
				K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	}
	for (int i = 0; i < CONC_THREADS; i++) {
		zassert_equal(k_sem_take(&conc_done, K_SECONDS(10)), 0);
	}
	zassert_equal(atomic_get(&conc_ok), 4, "并发不超日额余量");
	zassert_equal(entelea_guard_model_calls(), 4);
}
