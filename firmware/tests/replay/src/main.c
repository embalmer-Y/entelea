/* SPDX-License-Identifier: Apache-2.0 */
/*
 * L4 确定性重放雏形（LLD-ts-core §7 / testing.md §2；HLD M1 交付）。
 *
 * MA2 对接接口（LLD-agent-sim-harness §2 约定，随 M1 定稿）：
 *   输入 = 虚拟时钟驱动的脚本序列（G5/单元 H：输入文件实装——外部
 *   replay_script.tsv 在场则按脚本驱动，否则内建脚本 + 编译期 golden）；
 *   输出 = stdout JSONL 行（{"t_ms":..,"ch":..,"value_u":..}）+ 事件序列；
 *   判定 = ztest 断言（退出码表结果）。
 * 确定性（合同 9）：同一输入序列在两个相同通道上运行，写序列逐项一致，
 * 且与 golden 常量一致——"跑一遍拿输出当基准"是被禁止的（testing.md §2），
 * golden 的来源 = 规格推导（slew/限幅/状态机的数学预期）。
 */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/periph.h>
#include <ts/power.h>
#include <ts/safety.h>

static uint64_t virt_now;
static uint64_t virt_now_fn(void)
{
	return virt_now;
}
static const ts_time_source_t virt_src = {.now_ms = virt_now_fn};

/* 双通道同参数（slew 5/ms，min 0，max 1000，fault=7） */
#define REPLAY_CH(uid_literal)                                                     \
	{                                                                          \
		.uid = uid_literal, .kind = TS_CH_PWM, .poweron = {.u = 0},         \
		.linkloss = {.u = 0}, .fault = {.u = 7},                            \
		.limits = {.min = 0, .max = 1000, .slew_per_ms = 5},                \
	}

static const ts_out_ch_t ch_a = REPLAY_CH("rep_a");
static const ts_out_ch_t ch_b = REPLAY_CH("rep_b");

static int state_evt_seen;
static struct {
	uint64_t t_ms;
	const char *uid;
	ts_ch_state_t state;
} state_log[8];

static void state_cb(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(u);
	const ts_safe_state_evt_t *p = e->data;

	if (state_evt_seen < 8) {
		state_log[state_evt_seen].t_ms = e->t_ms;
		state_log[state_evt_seen].uid = p->uid;
		state_log[state_evt_seen].state = p->new_state;
	}
	state_evt_seen++;
}

/* 对单通道执行同一脚本（虚拟时间在外层统一推进） */
static void run_script(const char *uid)
{
	ts_out_value_t v = {.u = 0};

	virt_now = 200;
	v.u = 100;
	ts_safety_commit(uid, v); /* 基线 100 */
	virt_now = 210;
	v.u = 300;
	ts_safety_commit(uid, v); /* Δt=10 → 界 55 → 155（slew 拆分） */
	virt_now = 220;
	v.u = 0;
	ts_safety_commit(uid, v); /* Δt=10 → 界 55 → 100 */
}

/* golden：规格推导（slew 5/ms × Δt+1）+ estop fault 值 7 */
static const ts_write_rec_t golden[] = {
	{.t_ms = 200, .value_u = 100},
	{.t_ms = 210, .value_u = 155},
	{.t_ms = 220, .value_u = 100},
	{.t_ms = 230, .value_u = 7},
};

/* ---- G5（单元 H）：脚本文件会话（LLD-agent-sim-harness §2「输入文件进」接口实装）--------
 * 外部脚本 replay_script.tsv（cwd 相对）在场 → 按脚本驱动（runner 模式）；
 * 不在场 → 内建脚本全链（twister/CI 覆盖解析/执行/JSONL）。
 * 行格式（空白分隔）：<t_ms> link <0|1> | <t_ms> estop |
 *   <t_ms> input <inst> <mv>（注入→poll_once；变化才发 INPUT_CHANGED——首
 *   拍只建基线无回显，与固件 G3 传输级变化语义一致） |
 *   <t_ms> commit <ch> <value>（输出通道 = rep_c/rep_d，slew 5/ms 0..1000）。
 * JSONL = 输出通道写 + 输入回显（"in:<inst>"），按 t_ms 归并；同拍序 =
 * 输入回显 → rep_c → rep_d（因果序固定——确定性）。 */

#define SCRIPT_NAME     "replay_script.tsv"
#define SCRIPT_MAX_LINES 128
#define SCRIPT_LINE_LEN  96

static const ts_out_ch_t ch_c = REPLAY_CH("rep_c");
static const ts_out_ch_t ch_d = REPLAY_CH("rep_d");

static const ts_hal_dev_desc_t script_adc_devs[] = {
	{.uid = "adc0", .kind = TS_DEV_ADC},
	{.uid = "adc1", .kind = TS_DEV_ADC},
};

static struct {
	uint64_t t_ms;
	uint32_t inst;
	int32_t mv;
} in_echo[SCRIPT_MAX_LINES];
static size_t in_echo_n;

static void in_echo_cb(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(u);
	if (e->data == NULL || e->len < sizeof(struct ts_input_evt)) {
		return;
	}
	const struct ts_input_evt *p = e->data;

	if (in_echo_n < SCRIPT_MAX_LINES) {
		in_echo[in_echo_n].t_ms = e->t_ms;
		in_echo[in_echo_n].inst = p->inst;
		in_echo[in_echo_n].mv = p->new_mv;
	}
	in_echo_n++;
}

static void script_session_run(FILE *f)
{
	char line[SCRIPT_LINE_LEN];

	ts_time_test_bind(&virt_src);
	virt_now = 0;
	ts_safety_test_reset();
	for (size_t i = 0; i < sizeof(script_adc_devs) / sizeof(script_adc_devs[0]); i++) {
		(void)ts_hal_register_dev(&script_adc_devs[i]);
	}
	zassert_equal(ts_safety_register_channel(&ch_c), TS_OK);
	zassert_equal(ts_safety_register_channel(&ch_d), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_INPUT_CHANGED, in_echo_cb, NULL), TS_OK);
	in_echo_n = 0;

	unsigned long long t = 0;
	char op[16], a1[24];
	long long v = 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
			continue;
		}
		int n = sscanf(line, "%llu %15s %23s %lld", &t, op, a1, &v);

		if (n < 2) {
			zassert_true(false, "脚本行解析失败: %s", line);
			continue;
		}
		virt_now = t;
		if (strcmp(op, "link") == 0 && n >= 3) {
			ts_safety_set_link(atoi(a1) != 0);
		} else if (strcmp(op, "estop") == 0) {
			ts_safety_force_all_fault();
		} else if (strcmp(op, "input") == 0 && n >= 4) {
			ts_hal_input_test_inject((uint32_t)atoi(a1), (int32_t)v);
			ts_hal_input_poll_once();
		} else if (strcmp(op, "commit") == 0 && n >= 4) {
			ts_out_value_t ov = {.u = (uint32_t)v};

			(void)ts_safety_commit(a1, ov); /* 拒绝（estop 锁/未知 uid）
							 * = 无写——场景语义 */
		} else {
			zassert_true(false, "未知脚本操作: %s %s", op, a1);
		}
	}
	fclose(f);

	ts_write_rec_t wc[SCRIPT_MAX_LINES], wd[SCRIPT_MAX_LINES];
	size_t nc = ts_driversim_writes(ch_c.uid, wc, SCRIPT_MAX_LINES);
	size_t nd = ts_driversim_writes(ch_d.uid, wd, SCRIPT_MAX_LINES);
	size_t i = 0, j = 0, k = 0;

	while (i < nc || j < nd || k < in_echo_n) {
		uint64_t te = (k < in_echo_n) ? in_echo[k].t_ms : UINT64_MAX;
		uint64_t tc = (i < nc) ? wc[i].t_ms : UINT64_MAX;
		uint64_t td = (j < nd) ? wd[j].t_ms : UINT64_MAX;

		if (te <= tc && te <= td) {
			printk("{\"t_ms\":%llu,\"ch\":\"in:%u\",\"value_u\":%lld}\n",
			       (unsigned long long)in_echo[k].t_ms,
			       (unsigned)in_echo[k].inst,
			       (long long)in_echo[k].mv);
			k++;
		} else if (tc <= td) {
			printk("{\"t_ms\":%llu,\"ch\":\"%s\",\"value_u\":%u}\n",
			       (unsigned long long)wc[i].t_ms, ch_c.uid,
			       wc[i].value_u);
			i++;
		} else {
			printk("{\"t_ms\":%llu,\"ch\":\"%s\",\"value_u\":%u}\n",
			       (unsigned long long)wd[j].t_ms, ch_d.uid,
			       wd[j].value_u);
			j++;
		}
	}
	ts_time_test_bind(NULL);
}

ZTEST(framework_replay, test_00_script_file_session)
{
	FILE *f = fopen(SCRIPT_NAME, "r");
	bool builtin = (f == NULL);

	if (builtin) {
		/* twister/CI 路径：内建脚本落临时文件后走同一读取链（glibc 严格
		 * C 环境不露 fmemopen——文件路径即被测路径，等价且更真）*/
		static const char builtin_script[] =
			"100 link 1\n"
			"150 input 0 1200\n"
			"200 commit rep_c 100\n"
			"210 commit rep_c 300\n"
			"215 input 0 800\n"
			"220 commit rep_c 0\n"
			"230 estop\n";

		f = fopen("replay_builtin.tsv", "w+");
		zassert_not_null(f, "内建脚本临时文件");
		zassert_equal(fwrite(builtin_script, 1, sizeof(builtin_script) - 1, f),
			      sizeof(builtin_script) - 1, "内建脚本写入");
		rewind(f);
	}
	script_session_run(f);
	if (builtin) {
		/* 内建脚本 golden（规格推导：slew 5/ms ×(Δt+1) → 100/155/100/7；
		 * 输入 800@215 回显恰一次〔1200@150 为基线拍〕） */
		zassert_equal(in_echo_n, 1, "输入变化回显恰一次");
		zassert_equal(in_echo[0].t_ms, 215);
		zassert_equal(in_echo[0].mv, 800);
		ts_write_rec_t w[SCRIPT_MAX_LINES];
		size_t n = ts_driversim_writes(ch_c.uid, w, SCRIPT_MAX_LINES);

		zassert_equal(n, 4, "rep_c 写计数");
		const uint32_t want_v[4] = {100, 155, 100, 7};
		const uint64_t want_t[4] = {200, 210, 220, 230};
		for (size_t q = 0; q < n && q < 4; q++) {
			zassert_equal(w[q].value_u, want_v[q], "script golden v@%u", (unsigned)q);
			zassert_equal(w[q].t_ms, want_t[q], "script golden t@%u", (unsigned)q);
		}
	}
	/* 收尾清理（estop 锁存/link 态/通道注册表）——本会话先于 golden 运行，
	 * 其残留会改变 golden 的规格推导前提（estop 后写入锁死 = 0 写）。 */
	ts_safety_test_reset();
}

ZTEST(framework_replay, test_deterministic_replay)
{
	FILE *probe = fopen(SCRIPT_NAME, "r");

	if (probe != NULL) {
		fclose(probe);
		/* G5：外部脚本在场（runner 模式）——本会话让位：其 JSONL 输出会
		 * 混入脚本会话的采集流（评估以脚本产物为纯净对象）。twister
		 * 路径（无脚本）照常执行 golden。 */
		ztest_test_skip();
	}
	ts_time_test_bind(&virt_src);
	virt_now = 0;
	zassert_equal(ts_safety_register_channel(&ch_a), TS_OK);
	zassert_equal(ts_safety_register_channel(&ch_b), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_SAFE_STATE_CHANGED, state_cb, NULL), TS_OK);

	/* t=100 链路确立（两通道 POWERON→ACTIVE） */
	virt_now = 100;
	ts_safety_set_link(true);

	/* 同一脚本对两通道执行（同一输入序列） */
	run_script(ch_a.uid);
	run_script(ch_b.uid);

	/* t=230 estop 直达（两通道 → SAFE_FAULT，写 fault=7） */
	virt_now = 230;
	ts_safety_force_all_fault();

	/* 校验 1：两通道写序列逐项一致（确定性：同输入 → 同输出，合同 9） */
	ts_write_rec_t wa[8], wb[8];
	size_t na = ts_driversim_writes(ch_a.uid, wa, 8);
	size_t nb = ts_driversim_writes(ch_b.uid, wb, 8);

	zassert_equal(na, 4, "channel A write count");
	zassert_equal(na, nb, "identical sequences");
	for (size_t i = 0; i < na; i++) {
		zassert_equal(wa[i].t_ms, wb[i].t_ms, "time match at %u", (unsigned)i);
		zassert_equal(wa[i].value_u, wb[i].value_u, "value match at %u", (unsigned)i);
	}

	/* 校验 2：与规格推导的 golden 一致 */
	for (size_t i = 0; i < na; i++) {
		zassert_equal(wa[i].t_ms, golden[i].t_ms, "golden t at %u", (unsigned)i);
		zassert_equal(wa[i].value_u, golden[i].value_u, "golden v at %u", (unsigned)i);
	}

	/* 校验 3：estop 后写入锁死 */
	ts_out_value_t v = {.u = 500};

	zassert_equal(ts_safety_commit(ch_a.uid, v), TS_E_STATE);

	/* 校验 4：状态事件序列——estop/system_fail 路径不发布 SAFE_STATE_CHANGED
	 *（ISR 禁队列，合同 5），事后经 TS_EVT_ESTOP 补发；故此处仅 2 条（ACTIVE@100）。 */
	zassert_equal(state_evt_seen, 2, "link-up transitions only");
	for (int i = 0; i < 2; i++) {
		zassert_equal(state_log[i].t_ms, 100, "state evt time");
		zassert_equal(state_log[i].state, TS_ST_ACTIVE);
	}
	ts_ch_state_t st;
	zassert_equal(ts_safety_channel_state(ch_a.uid, &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_FAULT, "fault state visible (silent set)");

	/* 雏形接口：stdout JSONL（MA2 模拟器 harness 的采集格式） */
	for (size_t i = 0; i < na; i++) {
		printk("{\"t_ms\":%llu,\"ch\":\"%s\",\"value_u\":%u}\n",
		       (unsigned long long)wa[i].t_ms, ch_a.uid, wa[i].value_u);
	}
	ts_time_test_bind(NULL);
}

/* ---- M3b 集成重放：供电预算 + 插拔 → 写轨迹/事件 golden（合同 9）-------- */

static int integ_budget_evt, integ_detach_evt, integ_attach_evt;

static void integ_evt_cb(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(u);
	switch (e->id) {
	case TS_EVT_POWER_BUDGET:
		integ_budget_evt++;
		break;
	case TS_EVT_PERIPH_DETACH:
		integ_detach_evt++;
		break;
	case TS_EVT_PERIPH_ATTACH:
		integ_attach_evt++;
		break;
	default:
		break;
	}
}

/* 场景（虚拟时钟推进，规格推导 golden）：
 *   t=300 链路确立；t=310 pwr0 请求 150mA（写 enc(on,150)）
 *   t=320 pwr1 请求 200mA → 预算超（150+200>300）拒绝（无写）
 *   t=330 pwr0 关断（写 enc(off,0)）；t=340 DETACH → fault 写（off,0）
 *   t=350 ATTACH → 恢复 poweron 重放（off，poweron_on=false） */
ZTEST(framework_replay, test_m3b_power_hotplug_integration)
{
	static const ts_periph_desc_t pwr0 = {
		.uid = "ip0", .kind = TS_PK_POWER,
		.safe = {.uid = "ip0", .kind = TS_CH_POWER,
			 .poweron = {.pwr = {false, 0}},
			 .linkloss = {.pwr = {false, 0}},
			 .fault = {.pwr = {false, 0}},
			 .limits = {.current_limit_ma = 250}},
	};
	static const ts_periph_desc_t pwr1 = {
		.uid = "ip1", .kind = TS_PK_POWER,
		.safe = {.uid = "ip1", .kind = TS_CH_POWER,
			 .poweron = {.pwr = {false, 0}},
			 .linkloss = {.pwr = {false, 0}},
			 .fault = {.pwr = {false, 0}},
			 .limits = {.current_limit_ma = 250}},
	};
	ts_perm_table_t t;
	ts_ctx_t ctx;

	ts_time_test_bind(&virt_src);
	virt_now = 0;
	ts_safety_test_reset();
	ts_power_test_reset();
	ts_periph_test_reset();
	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("power:set:0-1", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 5, &t), TS_OK);
	zassert_equal(ts_periph_register(&pwr0), TS_OK);
	zassert_equal(ts_periph_register(&pwr1), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_POWER_BUDGET, integ_evt_cb, NULL), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_PERIPH_DETACH, integ_evt_cb, NULL), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_PERIPH_ATTACH, integ_evt_cb, NULL), TS_OK);
	ts_power_test_set_budget(300);
	integ_budget_evt = integ_detach_evt = integ_attach_evt = 0;

	virt_now = 300;
	ts_safety_set_link(true);
	virt_now = 310;
	zassert_equal(ts_power_request(ctx, 0, true, 150), TS_OK);
	virt_now = 320;
	zassert_equal(ts_power_request(ctx, 1, true, 200), TS_E_RANGE, "预算超拒绝");
	virt_now = 330;
	zassert_equal(ts_power_request(ctx, 0, false, 0), TS_OK);
	virt_now = 340;
	zassert_equal(ts_periph_detach("ip0"), TS_OK);
	virt_now = 350;
	zassert_equal(ts_periph_attach("ip0"), TS_OK);

	/* golden：ip0 写轨迹（规格推导——预算数学 + 三态值） */
	ts_write_rec_t w[8];
	size_t n = ts_driversim_writes("ip0", w, 8);
	const uint32_t ON150 = (1U << 31) | 150;
	const uint32_t OFF0 = 0;

	zassert_equal(n, 4, "ip0 write count");
	const uint32_t want_v[4] = {ON150, OFF0, OFF0, OFF0};
	const uint64_t want_t[4] = {310, 330, 340, 350};
	for (size_t i = 0; i < n && i < 4; i++) {
		zassert_equal(w[i].value_u, want_v[i], "integ golden v at %u", (unsigned)i);
		zassert_equal(w[i].t_ms, want_t[i], "integ golden t at %u", (unsigned)i);
	}
	/* ip1 零写（预算拒绝不留物理痕迹） */
	zassert_equal(ts_driversim_writes("ip1", w, 8), 0);

	/* 事件计数 golden */
	zassert_equal(integ_budget_evt, 1, "超预算事件恰一次");
	zassert_equal(integ_detach_evt, 1);
	zassert_equal(integ_attach_evt, 1);

	/* 预算快照终态：used=0（全关/重附 poweron=off），峰值 150 */
	ts_power_budget_t b;

	ts_power_budget_snapshot(&b);
	zassert_equal(b.used_ma, 0);
	zassert_equal(b.peak_ma, 150);
	zassert_equal(b.slots, 2);
	ts_time_test_bind(NULL);
}

#if defined(CONFIG_TS_APP_WAMR)
/* V2-3 sim APP 装载面：NV1 包（真机同包——nv.nvlab.fixloop.verify，
 * 测试根签；真机单迹 logs/nv1_PASS_console.txt）在 native_sim 装载运行。
 * 判据行（nv1 tick cnt=NN / NV1 DONE ticks=05）经 ts_log_write → printk
 * → stdout，由外部消费（sim_run verdict_patterns / 首基线对拍）——
 * ztest 内断言装载链与 APP 存活性（判据行机械断言归属 sim↔real 对拍面）。
 * 纯 log 计数型 APP = 双域兼容（无 ADC/camera 后端依赖）。 */
#include "nv1_pkg.h"

ZTEST(framework_replay, test_90_nv1_app_load)
{
	const uint8_t root_key[32] = {0}; /* TEST 构建：验签 = 固定测试根 */
	ts_app_info_t info;

	zassert_equal(ts_appmgr_install(nv1_pkg, sizeof(nv1_pkg),
					root_key, &info), TS_OK,
		      "install NV1 包（真机同包）");
	zassert_equal(ts_appmgr_boot_start(), TS_OK, "boot 装载");
	zassert_true(ts_appmgr_app_running(), "APP 运行中");

	/* APP 终态 = 计数 20 tick（100ms 周期）后 DONE 并停转；等待终态
	 * 判据行外部可见（判据行不在此断言——stdout 归 sim_run 对拍面）。
	 * 3.5s = 20 tick + 裕量；轮询终态（不再 running 或超时均收——
	 * APP 停转与否不阻塞装载链判定，判据行到达才是对拍判据）。 */
	for (int i = 0; i < 35 && ts_appmgr_app_running(); i++) {
		k_sleep(K_MSEC(100));
	}
	ts_app_info_t after;

	zassert_equal(ts_appmgr_get_info(&after), TS_OK);
	zassert_true(after.state == TS_APP_ACTIVE ||
		     after.state == TS_APP_STAGED, "终态在位");
}
#endif /* CONFIG_TS_APP_WAMR */

ZTEST_SUITE(framework_replay, NULL, NULL, NULL, NULL, NULL);
