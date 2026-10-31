/* SPDX-License-Identifier: Apache-2.0 */
/* framework.wamrdemo（Q-23 实证批，M2b.2a）：为宿主线程模型裁定提供实测数据。
 * 四实验（native_sim = 真实 pthread 抢占 + SMP 真并行 + 真实时间对齐）：
 *   test_01 时延画像   —— busy(n) 各档宿主耗时（判断"典型 APP 调用"量级）；
 *   test_02 B 方案实证 —— APP 跑在 sysworkq：周期任务被拖延多少（饿死）；
 *   test_03 竞态实证   —— 专用线程模型下现有无锁代码：预算 TOCTOU 实测
 *                          （信号量栅栏同步起跑，制造检查窗口对齐）；
 *   test_04 A 方案对照 —— APP 跑在低优先级专用线程：周期任务不受影响。
 * 断言仅做粗健全性（防优化消除/防死等）；测量值经 printk 输出供人工判读。
 * 注（如实标注）：native_sim 为 Linux 调度——真实 MCU 协作优先级语义下
 * test_02 的饿死只会更严重（下界证据），不会更轻。 */
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>
#include <wasm_export.h>
#include <ts/hal.h>
#include <ts/power.h>
#include <ts/safety.h>
#include "busy_wasm.h"

#define DEMO_PERIOD_US 10000u   /* 周期任务节拍 10ms（net_tick 量级代理） */
#define DEMO_TICKS     30u
#define BUSY_HEAVY     100000000u /* 重负载档 ≈105ms/次（实测 ~1.1ns/迭代） */

/* ---- 公共：runtime 生命周期与调用助手（exec_env 绑定创建线程——各上下文自建）---- */

static wasm_module_t demo_mod;
static bool demo_up;

/* 套件级一次生命周期：WAMR zephyr 平台层 init/destroy 不对称（同进程二次
 * init+load 实测失败）——demo 二进制不重复销毁，进程退出即回收。 */
static void demo_runtime_up(void)
{
	char err[128];

	if (demo_up) {
		return;
	}
	zassert_true(wasm_runtime_init(), "runtime init");
	demo_mod = wasm_runtime_load(busy_wasm, busy_wasm_len, err, sizeof(err));
	if (demo_mod == NULL) {
		printk("[wamrdemo] load fail: %s\n", err);
	}
	zassert_not_null(demo_mod, "load");
	demo_up = true;
}

static uint32_t demo_call_busy(wasm_module_inst_t inst, uint32_t n)
{
	wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 4096);
	wasm_function_inst_t fn;

	zassert_not_null(env, "exec env");
	fn = wasm_runtime_lookup_function(inst, "busy");
	zassert_not_null(fn, "lookup busy");
	uint32_t argv[1] = {n};
	bool ok = wasm_runtime_call_wasm(env, fn, 1, argv);

	if (!ok) {
		printk("[wamrdemo] EXCEPTION: %s\n",
		       wasm_runtime_get_exception(inst));
	}
	zassert_true(ok, "call busy");
	wasm_runtime_destroy_exec_env(env);
	return argv[0];
}

/* 宿主墙钟（native_sim 的 k_cycle_get_64 系模拟时钟——忙等不推进，
 * 测量宿主耗时须用真实时钟）。Zephyr minimal-libc 的 time.h 不声明
 * clock_gettime，但 native_sim 进程链接宿主 libc——显式声明使用之（仅
 * demo 测量代码，非产品路径）。 */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
extern int clock_gettime(int clock_id, struct timespec *tp);

static uint64_t ns_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- 周期任务节拍测量（每阶段独立实例——delayable 复用存在取消竞态）-------- */

struct tickmeter {
	struct k_work_delayable work;
	uint64_t max_late_ns;
	uint32_t count;
};

static void tm_handler(struct k_work *w)
{
	struct k_work_delayable *d = k_work_delayable_from_work(w);
	struct tickmeter *tm = CONTAINER_OF(d, struct tickmeter, work);
	static uint64_t prev_ns; /* 单一活动节拍器（各阶段串行使用） */

	uint64_t now = ns_now();

	if (prev_ns != 0) {
		uint64_t dt = now - prev_ns;
		uint64_t late = (dt > DEMO_PERIOD_US * 1000ull)
					? dt - DEMO_PERIOD_US * 1000ull : 0;

		if (late > tm->max_late_ns) {
			tm->max_late_ns = late;
		}
	}
	prev_ns = now;
	tm->count++;
	k_work_reschedule(&tm->work, K_USEC(DEMO_PERIOD_US));
}

static void tm_start(struct tickmeter *tm)
{
	memset(tm, 0, sizeof(*tm));
	k_work_init_delayable(&tm->work, tm_handler);
	k_work_reschedule(&tm->work, K_USEC(DEMO_PERIOD_US));
}

static void tm_stop(struct tickmeter *tm)
{
	struct k_work_sync sync;

	(void)k_work_cancel_delayable_sync(&tm->work, &sync);
}

static void tm_run(struct tickmeter *tm, uint32_t want)
{
	tm_start(tm);
	while (tm->count < want) {
		k_msleep(5);
	}
	tm_stop(tm);
}

/* ---- test_01：时延画像 ----------------------------------------------------- */

ZTEST(framework_wamrdemo, test_01_latency_profile)
{
	demo_runtime_up();
	char err[128];
	wasm_module_inst_t inst = wasm_runtime_instantiate(demo_mod, 4096, 8192,
							   err, sizeof(err));

	zassert_not_null(inst, "inst: %s", err);
	const struct { const char *label; uint32_t n; } cases[] = {
		{"busy(1)", 1}, {"busy(1e3)", 1000u},
		{"busy(1e5)", 100000u}, {"busy(1e6)", 1000000u},
		{"busy(1e8)", BUSY_HEAVY},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t ret = demo_call_busy(inst, cases[i].n); /* 预热一次 */
		uint64_t t0 = ns_now();

		ret = demo_call_busy(inst, cases[i].n);
		uint64_t ns = ns_now() - t0;
		printk("[wamrdemo-01] %-12s = %llu ns (ret=%u)\n", cases[i].label,
		       (unsigned long long)ns, ret);
	}
	wasm_runtime_deinstantiate(inst);
}

/* ---- test_02：B 方案实证（APP 与周期任务同队 sysworkq）---------------------- */

static struct k_work demo_busy_work;

static void busy_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	char err[128];
	wasm_module_inst_t inst = wasm_runtime_instantiate(demo_mod, 4096, 8192,
							   err, sizeof(err));

	if (inst == NULL) {
		printk("[wamrdemo-02] inst fail: %s\n", err);
		return;
	}
	(void)demo_call_busy(inst, BUSY_HEAVY);
	wasm_runtime_deinstantiate(inst);
}

ZTEST(framework_wamrdemo, test_02_starvation_on_sysworkq)
{
	demo_runtime_up();
	k_work_init(&demo_busy_work, busy_work_handler);

	static struct tickmeter tm_base, tm_load;

	tm_run(&tm_base, DEMO_TICKS);
	printk("[wamrdemo-02] 基线（无负载）max 迟到 = %llu ns\n",
	       (unsigned long long)tm_base.max_late_ns);

	/* 负载段：节拍运行中注入一次重 APP 工作项（与周期任务同队串行） */
	tm_start(&tm_load);
	k_work_submit(&demo_busy_work);
	while (tm_load.count < DEMO_TICKS) {
		k_msleep(5);
	}
	tm_stop(&tm_load);
	printk("[wamrdemo-02] B 方案（同队一次 busy(1e8)）max 迟到 = %llu ns"
	       "（基线 %llu）\n",
	       (unsigned long long)tm_load.max_late_ns,
	       (unsigned long long)tm_base.max_late_ns);
	zassert_true(tm_load.max_late_ns > tm_base.max_late_ns,
		     "同队负载应显著推迟周期任务（B 方案饿死证据）");
}

/* ---- test_03：预算 TOCTOU 竞态（同步起跑制造检查窗口对齐）------------------- */

#define RACE_ROUNDS 4000u

struct race_arg {
	ts_ctx_t ctx;
	uint8_t slot;
	uint32_t ma;
	uint32_t overshoot; /* 本人 ON 后立即快照，used > budget 计数 */
	uint32_t ok;
	uint32_t range;
	uint32_t other;
};

static struct k_sem race_ready_sem; /* 主线程收 2 */
static struct k_sem race_go_sem;    /* 双线程各收 1 */

static void race_entry(void *p1, void *p2, void *p3)
{
	struct race_arg *a = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (uint32_t i = 0; i < RACE_ROUNDS; i++) {
		k_sem_give(&race_ready_sem);
		k_sem_take(&race_go_sem, K_FOREVER); /* 栅栏：与对端同时冲 */

		ts_res_t r = ts_power_request(a->ctx, a->slot, true, a->ma);

		if (r == TS_OK) {
			a->ok++;
			ts_power_budget_t b;

			ts_power_budget_snapshot(&b);
			if (b.used_ma > b.budget_ma) {
				a->overshoot++;
			}
			(void)ts_power_request(a->ctx, a->slot, false, 0);
		} else if (r == TS_E_RANGE) {
			a->range++;
		} else {
			a->other++;
		}
	}
}

static K_THREAD_STACK_DEFINE(race_stack_a, 32768);
static K_THREAD_STACK_DEFINE(race_stack_b, 32768);
static struct k_thread race_th_a, race_th_b;

ZTEST(framework_wamrdemo, test_03_budget_toctou_race)
{
	static const ts_pwr_slot_t s0 = {.uid = "rb0", .current_limit_ma = 600};
	static const ts_pwr_slot_t s1 = {.uid = "rb1", .current_limit_ma = 600};
	ts_ctx_t ctx;
	ts_perm_table_t t;

	ts_safety_test_reset();
	ts_power_test_reset();
	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("power:set:0-3", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 7, &t), TS_OK);
	zassert_equal(ts_power_register_slot(&s0), TS_OK);
	zassert_equal(ts_power_register_slot(&s1), TS_OK);
	ts_power_test_set_budget(1000); /* 单请求 600 合法、双开 1200 超限 */
	ts_safety_set_link(true);

	struct race_arg a = {.ctx = ctx, .slot = 0, .ma = 600};
	struct race_arg b = {.ctx = ctx, .slot = 1, .ma = 600};

	/* 窗口上界测量：单线程全路径（检查→提交）耗时——TOCTOU 敞口的量化 */
	{
		uint64_t t0 = ns_now();

		for (uint32_t i = 0; i < 1000u; i++) {
			(void)ts_power_request(ctx, 0, true, 600);
			(void)ts_power_request(ctx, 0, false, 0);
		}
		uint64_t per = (ns_now() - t0) / 2000u;
		printk("[wamrdemo-03] 单请求全路径均值 = %llu ns（TOCTOU 窗口上界）\n",
		       (unsigned long long)per);
	}

	k_sem_init(&race_ready_sem, 0, 2);
	k_sem_init(&race_go_sem, 0, 2);
	k_thread_create(&race_th_a, race_stack_a, K_THREAD_STACK_SIZEOF(race_stack_a),
			race_entry, &a, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_create(&race_th_b, race_stack_b, K_THREAD_STACK_SIZEOF(race_stack_b),
			race_entry, &b, NULL, NULL, 5, 0, K_NO_WAIT);
	for (uint32_t r = 0; r < RACE_ROUNDS; r++) {
		k_sem_take(&race_ready_sem, K_FOREVER);
		k_sem_take(&race_ready_sem, K_FOREVER); /* 双方就位 */
		k_sem_give(&race_go_sem);
		k_sem_give(&race_go_sem);               /* 同时放行 */
	}
	k_thread_join(&race_th_a, K_SECONDS(60));
	k_thread_join(&race_th_b, K_SECONDS(60));
	printk("[wamrdemo-03] TOCTOU 实测（同步起跑 %u 轮，预算 1000/单请 600）："
	       "观测超限 %u + %u 次；结果分布 ok=%u/%u range=%u/%u other=%u/%u\n",
	       (unsigned)RACE_ROUNDS, a.overshoot, b.overshoot,
	       a.ok, b.ok, a.range, b.range, a.other, b.other);
	zassert_equal(a.ok + b.ok + a.range + b.range + a.other + b.other,
		      2u * RACE_ROUNDS, "全部轮次有结果（线程完整执行）");
}

/* ---- test_04：A 方案对照（低优先级专用线程 + 独立实例）--------------------- */

static K_THREAD_STACK_DEFINE(demo_app_stack, 32768);
static struct k_thread demo_app_th;
static struct k_sem demo_app_done;
static uint32_t demo_app_rounds;

static void app_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	char err[128];
	wasm_module_inst_t inst = wasm_runtime_instantiate(demo_mod, 4096, 8192,
							   err, sizeof(err));

	if (inst == NULL) {
		printk("[wamrdemo-04] inst fail: %s\n", err);
		k_sem_give(&demo_app_done);
		return;
	}
	for (uint32_t i = 0; i < 10; i++) {
		(void)demo_call_busy(inst, BUSY_HEAVY);
		demo_app_rounds++;
		k_msleep(20); /* 事件驱动形态：调用间隙让出 CPU（V1 APP 模型） */
	}
	wasm_runtime_deinstantiate(inst);
	k_sem_give(&demo_app_done);
}

ZTEST(framework_wamrdemo, test_04_dedicated_thread_control)
{
	demo_runtime_up();
	k_sem_init(&demo_app_done, 0, 1);
	demo_app_rounds = 0;

	static struct tickmeter tm;

	tm_start(&tm);
	k_tid_t th = k_thread_create(&demo_app_th, demo_app_stack,
				     K_THREAD_STACK_SIZEOF(demo_app_stack),
				     app_thread_entry, NULL, NULL, NULL, 10, 0,
				     K_NO_WAIT);
	zassert_not_null(th, "thread create");
	k_sem_take(&demo_app_done, K_FOREVER);
	tm_stop(&tm);
	printk("[wamrdemo-04] A 方案（专用线程 %u×busy(1e8)+20ms 间隙）max 迟到 = %llu ns"
	       "【native_sim 时间模型伪影：忙循环冻结模拟时钟，真实 MCU 上 ISR 抢占忙线程，"
	       "该值不可外推；本实验有效结论 = A 形态通路可用 + 周期任务未停摆】\n",
	       (unsigned)demo_app_rounds,
	       (unsigned long long)tm.max_late_ns);
	zassert_equal(demo_app_rounds, 10u, "APP 线程完整执行");
	zassert_true(tm.count > 0, "周期任务持续运转（未被饿死）");
}

ZTEST_SUITE(framework_wamrdemo, NULL, NULL, NULL, NULL, NULL);
