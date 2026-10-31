/* SPDX-License-Identifier: Apache-2.0 */
/* boardbench（板级二，效率 DoD 六项）——真机 xiao_esp32s3 计时基准。
 *
 * 方法论：宿主侧 k_cycle_get_64 计时（240MHz cycle 源，约 4.2ns 粒度）；
 * 完成检测 = 目标通道影子值翻转（ts_safety_readback 轮询，主线程已降
 * 优先级〔CONFIG_MAIN_THREAD_PRIORITY=15〕，APP 线程〔10〕投递即抢占）；
 * wasm 侧零测量逻辑（bench.c 只做受控负载 + 标记回写）。
 *   ① 解释器吞吐：BUSY N 迭代 → ns/iter（对照 native_sim Q-23 基线 ~1.1ns/iter）
 *   ② native 往返：ts_time_ms × N → ns/call（含解释循环开销，减 ① 基线得净往返）
 *   ③ 写路径端到端：ts_gpio_write × N → ns/call（perm→唯一写路径→驱动桩）
 *   ④ mailbox 时延分布：ECHO 采样 → min/p50/p95/max（µs）
 *   ⑥ 并发（SMP 双核）：压测线程直写独立通道期间重测 ④ + estop 全局强制
 *      生效性 + 恢复；⑤ 足迹为构建期数据（见 docs/board-bench.md）。
 * native_sim 上仅作构建/流程回归（模拟时钟忙循环下冻结——dev-env 教训 11，
 * 数字无意义只打印）。 */
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

/* Zephyr 4.4 实证（板级三批）：DT_HAS_PROP/DT_NODE_HAS_PROP 包装宏在 #if 下
 * 求值为假，而原始生成宏为真（P5 探针）——守卫直接用生成宏 + defined 保护
 * （native_sim 无 zephyr,user 节点 → 宏不存在 → defined() 拦截）。 */
#define BB_REAL_IO_COND defined(CONFIG_TS_DRV_GPIO) && \
	defined(DT_N_S_zephyr_user_P_uid_EXISTS) && DT_N_S_zephyr_user_P_uid_EXISTS
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include "app_wasm.h"

#define OP_BUSY 1u
#define OP_RT 2u
#define OP_WR 3u
#define OP_ECHO 4u
#define OP_WRIO 5u

#define CH_ECHO "be0"
#define CH_MARK "bm0"
#define CH_DATA "bd0"
#define CH_STRESS "bs0"
#define CH_IO "io0"

#ifdef CONFIG_BOARD_NATIVE_SIM
#define BUSY_ITERS 1000u
#define RT_N 200u
#define WR_N 100u
#define MB_SAMPLES 8u
#define CONC_ON 0
#else
/* BUSY_ITERS 配额耦合（批 A 收尾定案）：300000 iters ≈ 9M 指令超 DEC-48③
 * 指令配额 → wasm 异常 → APP 被健康链终止（BB1 静默死——evt_seen 停 1、
 * health_fails 累积、running=0；诊断探针实证）。配额引入（Q-28③A）晚于
 * 本 bench 定参且 4.4 期未复跑——非 4.5 回归（30000 档 1189ns/iter 与
 * 4.4 时代 1043ns 同量级 = 吞吐无回归）。30000 = 配额内最大量级样本。 */
#define BUSY_ITERS 30000u
#define RT_N 20000u
#define WR_N 5000u
#define MB_SAMPLES 300u
#define CONC_ON 1
#endif

#define POLL_BOUND 20000000u /* 轮询上限（含心跳打印）；超时 = FAIL 如实打印 */

/* 计时源：Xtensa CCOUNT（CPU 时钟直接驱动）。
 * 实证（板级二）：esp32s3 上 k_cycle_get_64 冻结（75ms 忙等 delta=0），
 * uptime 正常——CCOUNT 为本板唯一可靠周期源。32 位 @240MHz 约 17.8s 回绕，
 * 各阶段差值即时计算（< 4s）回绕安全。native_sim 回退 k_cycle（流程回归
 * 模式，模拟时钟冻结下数字无意义——dev-env 教训 11）。 */
#ifdef __XTENSA__
static inline uint32_t cyc_now(void)
{
	uint32_t v;

	__asm__ volatile("rsr.ccount %0" : "=r"(v));
	return v;
}
#else
static inline uint32_t cyc_now(void)
{
	return (uint32_t)k_cycle_get_64();
}
#endif

static uint64_t cyc_to_ns(uint32_t delta)
{
	return (uint64_t)delta * 1000000000ULL / sys_clock_hw_cycles_per_sec();
}

static const ts_out_ch_t ch_echo = {
	.uid = CH_ECHO, .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_out_ch_t ch_mark = {
	.uid = CH_MARK, .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_out_ch_t ch_data = {
	.uid = CH_DATA, .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_out_ch_t ch_stress = {
	.uid = CH_STRESS, .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_out_ch_t ch_io = {
	.uid = CH_IO, .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};

/* ---- BB5 真机 IO 延迟（DEC-44）：三层对照 + 分布 -------------------------- */
#if BB_REAL_IO_COND
#define IO_N 10000u
static const struct gpio_dt_spec io_spec =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), io_gpios);
static void sort_u32(uint32_t *a, size_t n);

static uint32_t io_ns[IO_N];

static void raw_fn(uint32_t i)
{
	gpio_pin_set_dt(&io_spec, i & 1u);
}

static void fw_fn(uint32_t i)
{
	ts_out_value_t v = {.b = (i & 1u) != 0};

	(void)ts_safety_commit(CH_IO, v);
}

static void io_dist(const char *tag, uint32_t n, void (*fn)(uint32_t))
{
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c0 = cyc_now();

		fn(i);
		io_ns[i] = (uint32_t)cyc_to_ns(cyc_now() - c0);
	}
	sort_u32(io_ns, n);
	printk("BB5 %s n=%u min=%u p50=%u p95=%u max=%u ns p50_hz=%u\n", tag, n,
	       io_ns[0], io_ns[n / 2], io_ns[n * 95u / 100u], io_ns[n - 1],
	       (uint32_t)(1000000000ULL / io_ns[n / 2]));
}
#endif /* CONFIG_TS_DRV_GPIO */

static bool rb(const char *uid)
{
	ts_out_value_t v;

	if (ts_safety_readback(uid, &v) != TS_OK) {
		return false;
	}
	return v.b;
}

/* 自 c0 起等待 uid 影子值变为 want；成功回填 dt_ns。 */
static bool wait_flip(const char *uid, bool want, uint32_t c0, uint64_t *dt_ns)
{
	for (uint32_t i = 0; i < POLL_BOUND; i++) {
		if (rb(uid) == want) {
			*dt_ns = cyc_to_ns(cyc_now() - c0);
			return true;
		}
		if ((i % 1000000u) == 999999u) {
			printk("."); /* 心跳：挂起定位 */
		}
	}
	printk("\nBB POLL TIMEOUT uid=%s want=%d\n", uid, want);
	return false;
}

/* 投递基准命令并等完成标记翻转；返回总耗时 ns。 */
static uint64_t post_and_wait(uint32_t op, uint32_t n, const char *uid,
			      bool want, bool *ok)
{
	uint32_t v = op | ((uint32_t)want << 4) | (n << 5);
	uint64_t dt = 0;
	uint32_t c0 = cyc_now();
	ts_res_t r = ts_appmgr_app_evt(v);

	*ok = (r == TS_OK) && wait_flip(uid, want, c0, &dt);
	return dt;
}

static void sort_u32(uint32_t *a, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		uint32_t k = a[i];
		size_t j = i;
		while (j > 0 && a[j - 1] > k) {
			a[j] = a[j - 1];
			j--;
		}
		a[j] = k;
	}
}

static uint32_t mb_dist(const char *tag, uint32_t samples)
{
	static uint32_t us[512];
	uint32_t n = samples < 512u ? samples : 512u;
	bool cur = rb(CH_ECHO), ok = true;
	uint32_t fails = 0;

	for (uint32_t s = 0; s < n; s++) {
		bool step;
		uint64_t dt = post_and_wait(OP_ECHO, 0, CH_ECHO, !cur, &step);

		if (!step) {
			fails++;
		}
		us[s] = (uint32_t)(dt / 1000u);
		cur = !cur;
	}
	(void)ok;
	sort_u32(us, n);
	printk("BB4 %s n=%u fails=%u min=%u p50=%u p95=%u max=%u us\n", tag, n,
	       fails, us[0], us[n / 2], us[n * 95u / 100u], us[n - 1]);
	return fails;
}

#if CONC_ON
static struct k_thread stress_thr;
static K_THREAD_STACK_DEFINE(stress_stack, 4096);
static atomic_t stress_run;

static void stress_fn(void *a, void *b, void *c)
{
	unsigned i = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (atomic_get(&stress_run)) {
		ts_out_value_t v = {.b = (i++ & 1u) != 0};

		ts_safety_commit(CH_STRESS, v);
	}
}
#endif

int main(void)
{
	bool ok;
	bool mark = false;

	printk("BB0 start smp=%d cpus=%d cyc_hz=%u\n", IS_ENABLED(CONFIG_SMP),
	       (int)CONFIG_MP_MAX_NUM_CPUS, sys_clock_hw_cycles_per_sec());
	{ /* 计时源自检：CCOUNT 必须走时（esp32s3 实证批） */
		volatile uint32_t spin = 0;
		uint32_t c0 = cyc_now();
		uint32_t u0 = k_uptime_get_32();

		for (uint32_t i = 0; i < 2000000u; i++) {
			spin += i;
		}
		uint32_t dc = cyc_now() - c0;
		uint32_t du = k_uptime_get_32() - u0;

		printk("BB0 timesrc check spin_sink=%u cyc_delta=%u up_ms=%u\n",
		       spin, dc, du);
	}

	/* 通道描述符必须各自独立持久（注册存指针——栈上复用单对象 = 全表别名，
	 * 板级二实证教训：四通道全指向同一 uid，ts_ch_find 全 NOTFOUND） */
	static const ts_out_ch_t *const chs[5] = {
		&ch_echo, &ch_mark, &ch_data, &ch_stress, &ch_io,
	};
	static const ts_hal_dev_desc_t devs[4] = {
		{.uid = CH_ECHO, .kind = TS_DEV_GPIO_OUT},
		{.uid = CH_MARK, .kind = TS_DEV_GPIO_OUT},
		{.uid = CH_DATA, .kind = TS_DEV_GPIO_OUT},
		{.uid = CH_IO, .kind = TS_DEV_GPIO_OUT},
	};

	for (int i = 0; i < 4; i++) {
		if (ts_hal_register_dev(&devs[i]) != TS_OK) {
			printk("BB FAIL hal_register %d\n", i);
			return 1;
		}
	}
	printk("BBs devs ok\n");
	for (int i = 0; i < 5; i++) {
		if (ts_safety_register_channel(chs[i]) != TS_OK) {
			printk("BB FAIL ch_register %d\n", i);
			return 1;
		}
	}
	ts_safety_set_link(true);
#if BB_REAL_IO_COND
	if (ts_drv_gpio_init() != 0) {
		printk("BB FAIL ts_drv_gpio_init\n");
		return 1;
	}
	printk("BBs real-io ok (uid=%s)\n", CH_IO);
#endif
	printk("BBs channels ok\n");

	uint32_t c0 = cyc_now();
	ts_res_t r = ts_appmgr_app_start(1, bench_wasm, bench_wasm_len,
					 "gpio:write:0-3");
	printk("BBs app_start r=%d\n", r);
	if (r != TS_OK) {
		printk("BB FAIL app_start r=%d\n", r);
		return 1;
	}
	uint64_t dt;
	if (!wait_flip(CH_MARK, true, c0, &dt)) {
		struct ts_app_rt_stats st;

		ts_appmgr_app_stats(&st);
		printk("\nBB FAIL app_init marker (init_res=%d running=%d "
		       "health_fails=%u evt_seen=%u)\n", (int)st.init_res,
		       st.running, st.health_fails, st.evt_seen);
		return 1;
	}
	mark = true;
	printk("BB-init load+instantiate+init=%llu us\n", dt / 1000);

	/* ① 解释器吞吐 */
	dt = post_and_wait(OP_BUSY, BUSY_ITERS, CH_MARK, !mark, &ok);
	printk("BB1 busy iters=%u ok=%d total_ns=%llu per_iter=%u\n",
	       BUSY_ITERS, ok, dt, ok ? (uint32_t)(dt / BUSY_ITERS) : 0u);
	if (!ok) { printk("BB FAIL busy\n"); return 1; }
	mark = !mark;

	/* ② native 往返 */
	dt = post_and_wait(OP_RT, RT_N, CH_MARK, !mark, &ok);
	printk("BB2 rt n=%u ok=%d total_ns=%llu per_call_ns=%u\n",
	       RT_N, ok, dt, ok ? (uint32_t)(dt / RT_N) : 0u);
	if (!ok) { printk("BB FAIL rt\n"); return 1; }
	mark = !mark;

	/* ③ 写路径端到端 */
	dt = post_and_wait(OP_WR, WR_N, CH_MARK, !mark, &ok);
	printk("BB3 wr n=%u ok=%d total_ns=%llu per_call_ns=%u\n",
	       WR_N, ok, dt, ok ? (uint32_t)(dt / WR_N) : 0u);
	if (!ok) { printk("BB FAIL wr\n"); return 1; }
	mark = !mark;

	/* ④ mailbox 时延分布（空载基线） */
	if (mb_dist("idle", MB_SAMPLES) != 0) {
		printk("BB FAIL mb idle\n");
		return 1;
	}

	/* ⑤' BB5 真机 IO 延迟（DEC-44）：裸 Zephyr / 框架安全层 / wasm 全路径 */
#if BB_REAL_IO_COND
	io_dist("raw", IO_N, raw_fn);
	io_dist("fw", IO_N, fw_fn);
	dt = post_and_wait(OP_WRIO, 5000, CH_MARK, !mark, &ok);
	printk("BB5 wasm n=5000 ok=%d total_ns=%llu per_call_ns=%u\n", ok, dt,
	       ok ? (uint32_t)(dt / 5000u) : 0u);
	if (!ok) {
		printk("BB FAIL wrio\n");
		return 1;
	}
	mark = !mark;
#else
	printk("BB5 skipped（无真机 GPIO 后端）\n");
#endif

#if CONC_ON
	/* ⑥ 并发（单核抢占模式）：压测线程直写独立通道期间重测 ④ + estop 生效性。
	 * stress 优先级 = main（15）——同抢占级靠时间片轮转，单核上高于 stress
	 * 的纯抢占会饿死观测面（实证：prio 9 时下方全饿死无声）；APP(10) 高于
	 * 两者仍可抢占，write_lock 竞争真实可见。 */
	atomic_set(&stress_run, 1);
	k_thread_create(&stress_thr, stress_stack, K_THREAD_STACK_SIZEOF(stress_stack),
			stress_fn, NULL, NULL, NULL, 15, 0, K_NO_WAIT);
	k_thread_name_set(&stress_thr, "bench_stress");

	uint32_t f = mb_dist("cont", 100);
#if BB_REAL_IO_COND
	io_dist("fw_cont", 2000, fw_fn); /* 压测下真机 IO 写分布 */
#endif
	ts_safety_force_all_fault();
	bool all_safe = !rb(CH_ECHO) && !rb(CH_MARK) && !rb(CH_DATA) &&
			!rb(CH_STRESS);
	printk("BB6 estop_in_contention all_safe=%d mb_fails=%u\n", all_safe, f);

	ts_safety_clear_fault();
	ts_safety_channel_recover(CH_ECHO);
	ts_safety_channel_recover(CH_MARK);
	ts_safety_channel_recover(CH_DATA);
	ts_safety_channel_recover(CH_STRESS);
	ts_safety_set_link(true);

	atomic_set(&stress_run, 0);
	k_thread_join(&stress_thr, K_MSEC(2000));

	bool echo_cur = rb(CH_ECHO);
	uint64_t d2 = post_and_wait(OP_ECHO, 0, CH_ECHO, !echo_cur, &ok);
	printk("BB6 recovered echo_ok=%d dt_us=%llu\n", ok, d2 / 1000);
	if (!ok || !all_safe) {
		printk("BB FAIL conc\n");
		return 1;
	}
#else
	printk("BB6 skipped (native_sim 流程回归模式)\n");
#endif

	printk("BB running=%d\nBB PASS\n", ts_appmgr_app_running());
	return 0;
}
