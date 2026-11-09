/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R02 输出保护 L1 测试（迁自旧 firmware/tests/safety/src/main.c，oracle
 * 语义保留；审计环形（R03）与控制源仲裁（R04）子集未迁入，对应断言裁掉；
 * 新增单通道 detach/recover 机制面用例（旧 T-PERIPH 语义的机制子集））。
 * 有状态顺序场景：合并为单一用例断言链 = 规格链（ztest 用例序不保证）。
 */
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>
#include <stdio.h>
#include <entelea/events.h>
#include <entelea/safety.h>

static uint64_t virt_now;
static bool clock_fail_next;   /* 一次性读失败注入 */
static bool clock_fail_persist; /* 持续失败模式（二轮 review：持续故障类别） */

static entelea_result_t virt_read_ms(void *user, uint64_t *out)
{
	ARG_UNUSED(user);
	if (clock_fail_next) {
		clock_fail_next = false;
		return ENTELEA_E_IO;
	}
	if (clock_fail_persist) {
		return ENTELEA_E_IO;
	}
	*out = virt_now;
	return ENTELEA_OK;
}

static entelea_clock_t virt_clock = {
	.read_ms = virt_read_ms,
	.user = NULL,
	.clock_id = 0x5AFE,
	.last_ms = 0,
	.sampled = false,
};

static entelea_result_t isr_commit_res;

static void isr_commit_fn(const void *arg)
{
	const char *uid = arg;
	entelea_out_value_t v = {.u = 500};

	isr_commit_res = entelea_safety_commit(uid, v);
}

ZTEST(framework_safety, test_safety_full_scenario)
{
	entelea_out_value_t v = {.u = 0};
	entelea_out_value_t rb = {0};
	entelea_ch_state_t st;

	entelea_safety_test_reset(); /* 容量算术敏感（fill 至 32）——显式净基线 */
	zassert_equal(entelea_safety_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 0;

	/* ---- 1. 注册校验：三态缺一/值越限/uid 非法 → 拒绝，不留半注册 ---- */
	zassert_equal(entelea_safety_register_channel(NULL), ENTELEA_E_PARAM);
	const entelea_out_channel_t over_max = {
		.uid = "bad1", .kind = ENTELEA_CH_PWM,
		.poweron = {.u = 2000}, .linkloss = {.u = 0}, .fault = {.u = 0},
		.limits = {.min = 0, .max = 1000},
	};
	zassert_equal(entelea_safety_register_channel(&over_max), ENTELEA_E_PARAM,
		      "poweron 超 max 拒绝");

	static const entelea_out_channel_t pwm_ch = { /* min=100 限幅用例 */
		.uid = "pwm1", .kind = ENTELEA_CH_PWM,
		.poweron = {.u = 100}, .linkloss = {.u = 100}, .fault = {.u = 100},
		.limits = {.min = 100, .max = 1000},
	};
	static const entelea_out_channel_t slew_ch = { /* slew 用例：5/ms */
		.uid = "slew1", .kind = ENTELEA_CH_PWM,
		.poweron = {.u = 0}, .linkloss = {.u = 0}, .fault = {.u = 0},
		.limits = {.min = 0, .max = 1000, .slew_per_ms = 5},
	};
	static const entelea_out_channel_t pwr_ch = { /* 受控供电：限流 500mA */
		.uid = "pwr1", .kind = ENTELEA_CH_POWER,
		.poweron = {.pwr = {.en = false, .ma = 0}},
		.linkloss = {.pwr = {.en = false, .ma = 0}},
		.fault = {.pwr = {.en = false, .ma = 0}},
		.limits = {.current_limit_ma = 500},
	};
	static const entelea_out_channel_t det_ch = { /* detach/recover 用例 */
		.uid = "det1", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = true},
	};
	zassert_equal(entelea_safety_register_channel(&pwm_ch), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&slew_ch), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&pwr_ch), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&det_ch), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&pwm_ch), ENTELEA_E_PARAM,
		      "uid 撞名拒绝");

	/* ---- 2. 状态机与写路径保护 ---- */
	/* 非 ACTIVE（SAFE_POWERON）写入被拒。 */
	v.u = 500;
	zassert_equal(entelea_safety_commit("pwm1", v), ENTELEA_E_STATE,
		      "commit before link");

	entelea_safety_set_link(true); /* 链路确立 → ACTIVE */
	zassert_equal(entelea_safety_channel_state("pwm1", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_ACTIVE, "POWERON -> ACTIVE");

	/* 限幅：commit 20 → clamp min=100，res=E_RANGE。 */
	v.u = 20;
	zassert_equal(entelea_safety_commit("pwm1", v), ENTELEA_E_RANGE);
	zassert_equal(entelea_safety_readback("pwm1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 100, "clamped to min");

	/* 断链 → SAFE_LINKLOSS：写入拒 + shadow=安全值；恢复不自动回写。 */
	entelea_safety_set_link(false);
	zassert_equal(entelea_safety_channel_state("pwm1", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_LINKLOSS);
	v.u = 500;
	zassert_equal(entelea_safety_commit("pwm1", v), ENTELEA_E_STATE,
		      "commit in linkloss");
	zassert_equal(entelea_safety_readback("pwm1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 100, "shadow = linkloss value");
	entelea_safety_set_link(true);
	zassert_equal(entelea_safety_readback("pwm1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 100, "恢复不回写断链前值");

	/* slew 数学：slew1 5/ms，总预算锚定最后确认写（三轮 review 发现 4：
	 * 严格界 = slew×dt，无每笔 +1 免跳）。 */
	virt_now = 1000;
	v.u = 100;
	zassert_equal(entelea_safety_commit("slew1", v), ENTELEA_OK); /* 基线 */
	virt_now = 1010; /* Δt=10 → 界 = 5×10 = 50 */
	v.u = 150;
	zassert_equal(entelea_safety_commit("slew1", v), ENTELEA_OK, "within bound");
	virt_now = 1011; /* Δt=1 → 界 = 5×1 = 5 */
	v.u = 300;
	zassert_equal(entelea_safety_commit("slew1", v), ENTELEA_E_RANGE,
		      "over slew -> split");
	zassert_equal(entelea_safety_readback("slew1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 155, "split to bound 150+5");

	/* ---- 2b. slew 时钟门（二轮 review 发现 1）：本次读失败=时间不可知
	 * → 速率界不可立 → 整笔拒绝；持续失败下重复提交不得逐步改变输出
	 * （修复前：护栏 dt=0 → 每笔一跳，可逐步走满）。 ---- */
	virt_now = 1011; /* 上一确认写 last_t = 1011（shadow 155） */
	clock_fail_persist = true; /* 持续失败：重复提交不得逐步改变输出 */
	for (int i = 0; i < 3; i++) {
		zassert_equal(
			entelea_safety_commit("slew1", (entelea_out_value_t){.u = 300}),
			ENTELEA_E_STATE, "unreadable clock must reject slew commits");
		zassert_equal(entelea_safety_readback("slew1", &rb), ENTELEA_OK);
		zassert_equal(rb.u, 155, "输出不得在时钟失明期间改变");
	}
	clock_fail_persist = false;
	zassert_true(entelea_safety_clock_faults() >= 3, "failures counted");
	/* 时钟恢复后正常计量：dt=10 → 界 50 → 拆到 155+50=205。 */
	virt_now = 1021;
	v.u = 400;
	zassert_equal(entelea_safety_commit("slew1", v), ENTELEA_E_RANGE);
	zassert_equal(entelea_safety_readback("slew1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 205, "bound 155+5*10 (dt=10 after recovery)");

	/* ---- 3. 限流（POWER）：请求超限整笔拒绝 ---- */
	v.pwr.en = true;
	v.pwr.ma = 600;
	zassert_equal(entelea_safety_commit("pwr1", v), ENTELEA_E_RANGE);
	zassert_equal(entelea_safety_readback("pwr1", &rb), ENTELEA_OK);
	zassert_false(rb.pwr.en, "over-limit rejected entirely");
	v.pwr.ma = 400;
	zassert_equal(entelea_safety_commit("pwr1", v), ENTELEA_OK);
	zassert_equal(entelea_safety_readback("pwr1", &rb), ENTELEA_OK);
	zassert_true(rb.pwr.en);
	zassert_equal(rb.pwr.ma, 400);

	/* ---- 4. 单通道 detach/recover（物理不在场 = 故障态；重附 = 上电态） ---- */
	zassert_equal(entelea_safety_force_channel_fault("noperf"), ENTELEA_E_NOTFOUND);
	zassert_equal(entelea_safety_force_channel_fault("det1"), ENTELEA_OK);
	zassert_equal(entelea_safety_channel_state("det1", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT, "detach -> SAFE_FAULT");
	zassert_equal(entelea_safety_commit("det1", (entelea_out_value_t){.b = false}),
		      ENTELEA_E_STATE, "detached channel rejects writes");
	zassert_equal(entelea_safety_readback("det1", &rb), ENTELEA_OK);
	zassert_true(rb.b, "fault value applied");
	zassert_equal(entelea_safety_channel_recover("det1"), ENTELEA_OK);
	zassert_equal(entelea_safety_channel_state("det1", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_ACTIVE, "recover -> ACTIVE (link up)");
	zassert_equal(entelea_safety_readback("det1", &rb), ENTELEA_OK);
	zassert_false(rb.b, "recover 回 poweron 值");

	/* ---- 5. ISR 调 [thread] API 拒绝 ---- */
	v.u = 500;
	zassert_equal(entelea_safety_commit("pwm1", v), ENTELEA_OK, "前置：ACTIVE 可写");
	isr_commit_res = ENTELEA_OK;
	irq_offload(isr_commit_fn, "pwm1");
	zassert_equal(isr_commit_res, ENTELEA_E_PERM, "ISR 调 commit 必须被拒");

	/* ---- 6. 容量 32：初始 4 + 填 28 = 满，NOMEM ---- */
	static entelea_out_channel_t fill[28];
	static char names[28][16]; /* 静态存储：uid 必须指向持久内存 */

	for (int i = 0; i < 28; i++) {
		snprintf(names[i], sizeof(names[i]), "fill%02d", i);
		fill[i] = pwm_ch;
		fill[i].uid = names[i];
		zassert_equal(entelea_safety_register_channel(&fill[i]), ENTELEA_OK,
			      "fill %d", i);
	}
	static const entelea_out_channel_t one_more = {
		.uid = "overflow1", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	zassert_equal(entelea_safety_register_channel(&one_more), ENTELEA_E_NOMEM,
		      "registry full");

	/* ---- 7. estop 直达 + 锁存与释放（clear 是唯一释放路径） ---- */
	entelea_safety_force_all_fault();
	/* 观测面 getter（r14）：闩位与触发时刻（virt_now=1021 段）。 */
	zassert_true(entelea_safety_estop_latched(), "estop latched observable");
	zassert_equal(entelea_safety_estop_latched_at(), 1021,
		      "estop latched_at observable");
	zassert_equal(entelea_safety_channel_state("pwm1", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT);
	zassert_equal(entelea_safety_commit("pwm1", v), ENTELEA_E_STATE,
		      "commit locked after estop");
	zassert_equal(entelea_safety_readback("pwm1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 100, "fault value applied");
	/* 全局锁存期单通道恢复拒绝。 */
	zassert_equal(entelea_safety_channel_recover("pwm1"), ENTELEA_E_STATE,
		      "recover rejected while forced");

	/* estop-clear：释放锁存 → 条件恢复（link up → ACTIVE）→ 写路径恢复。 */
	zassert_equal(entelea_safety_clear_fault(), ENTELEA_OK);
	zassert_false(entelea_safety_estop_latched(), "clear releases latch");
	zassert_equal(entelea_safety_estop_latched_at(), 1021,
		      "clear keeps last-trigger record (forced_at)");
	zassert_equal(entelea_safety_channel_state("pwm1", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_ACTIVE, "conditional recovery to ACTIVE");
	zassert_equal(entelea_safety_readback("pwm1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 100, "clear 不回写：保持 fault 安全值直至显式 commit");
	v.u = 300;
	zassert_equal(entelea_safety_commit("pwm1", v), ENTELEA_OK,
		      "write path restored after clear");

	/* 二次 estop 可再次锁存（announce 复位）。 */
	entelea_safety_force_all_fault();
	zassert_equal(entelea_safety_channel_state("pwm1", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT, "re-latch after clear");
}

ZTEST_SUITE(framework_safety, NULL, NULL, NULL, NULL, NULL);

/* ---- estop 边沿映射（纯函数守卫；prov 化配置面归 R03） ---- */
ZTEST(framework_safety, test_estop_edge_mapping)
{
	zassert_equal(entelea_safety_estop_edge_of(0), GPIO_INT_EDGE_RISING,
		      "缺省 = 上升（兼容既有）");
	zassert_equal(entelea_safety_estop_edge_of(1), GPIO_INT_EDGE_FALLING);
	zassert_equal(entelea_safety_estop_edge_of(2), GPIO_INT_EDGE_BOTH);
	zassert_equal(entelea_safety_estop_edge_of(99), GPIO_INT_EDGE_RISING,
		      "未知 = fail-safe 上升");
}

/* ---- poweron 值落驱动（注册 → poweron_init：物理写序列第一笔 = 声明值） ---- */
/* ---- slew 总预算（三轮 review 发现 4）：同一毫秒内重复提交的
 * 总变化 ≤ slew/ms——修复前每笔 dt+1 免跳，200 笔同类提交可 0→200。 */
ZTEST(framework_safety, test_slew_total_budget_per_ms)
{
	entelea_safety_test_reset();
	zassert_equal(entelea_safety_bind_clock(&virt_clock), ENTELEA_OK);
	static const entelea_out_channel_t rate1 = {
		.uid = "rate1", .kind = ENTELEA_CH_PWM,
		.poweron = {.u = 0}, .linkloss = {.u = 0}, .fault = {.u = 0},
		.limits = {.min = 0, .max = 1000, .slew_per_ms = 1},
	};
	zassert_equal(entelea_safety_register_channel(&rate1), ENTELEA_OK);
	entelea_safety_set_link(true);

	virt_now = 5000;
	zassert_equal(entelea_safety_commit("rate1", (entelea_out_value_t){.u = 0}),
		      ENTELEA_OK); /* 基线：shadow 0, last_t 5000 */
	/* 同一毫秒 200 笔上跳请求：每笔 dt=0 → 界 0 → 拆分回当前值。 */
	for (int i = 0; i < 200; i++) {
		zassert_equal(
			entelea_safety_commit("rate1", (entelea_out_value_t){.u = 200}),
			ENTELEA_E_RANGE, "same-ms commits must not move the output");
	}
	entelea_out_value_t rb;

	zassert_equal(entelea_safety_readback("rate1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 0, "total same-ms movement must be 0 (was 200 pre-fix)");
	/* 下一毫秒：预算 1/ms。 */
	virt_now = 5001;
	zassert_equal(entelea_safety_commit("rate1", (entelea_out_value_t){.u = 5}),
		      ENTELEA_E_RANGE);
	zassert_equal(entelea_safety_readback("rate1", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 1, "next-ms budget = slew*1");
}

/* ---- 基线跨迁移持续（决定 C）：任何确认写刷新锚点，免跳面关闭 ---- */
ZTEST(framework_safety, test_slew_baseline_persists_across_transitions)
{
	entelea_safety_test_reset();
	zassert_equal(entelea_safety_bind_clock(&virt_clock), ENTELEA_OK);
	static const entelea_out_channel_t ramp = {
		.uid = "ramp", .kind = ENTELEA_CH_PWM,
		.poweron = {.u = 0}, .linkloss = {.u = 0}, .fault = {.u = 0},
		.limits = {.min = 0, .max = 1000, .slew_per_ms = 5},
	};
	zassert_equal(entelea_safety_register_channel(&ramp), ENTELEA_OK);

	/* poweron_init = 确认写（0@t0）：首笔提交即受 slew 限。 */
	virt_now = 0;
	zassert_equal(entelea_safety_poweron_init(), ENTELEA_OK);
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);
	virt_now = 10; /* 界 = 5×10 = 50 */
	zassert_equal(entelea_safety_commit("ramp", (entelea_out_value_t){.u = 1000}),
		      ENTELEA_E_RANGE, "首笔从确认基线爬升（决定 C）");
	entelea_out_value_t rb;

	zassert_equal(entelea_safety_readback("ramp", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 50, "split to 0+50");

	/* estop→clear 循环不再产生免跳：fault 写=确认写刷新锚点，
	 * clear 无物理写保留基线，随后同毫秒提交界=0。 */
	virt_now = 100;
	zassert_equal(entelea_safety_commit("ramp", (entelea_out_value_t){.u = 100}),
		      ENTELEA_OK); /* 界 5×90=450 ≥ 100−50：整笔通过 */
	entelea_safety_force_all_fault(); /* fault=0 确认写 @t=100 */
	zassert_equal(entelea_safety_clear_fault(), ENTELEA_OK);
	zassert_equal(entelea_safety_commit("ramp", (entelea_out_value_t){.u = 1000}),
		      ENTELEA_E_RANGE, "estop-clear 循环不得免跳");
	zassert_equal(entelea_safety_readback("ramp", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 0, "循环后同毫秒界=0（修复前可免跳至 1000）");

	/* recover=确认写（poweron 值@t2）：其后提交从该值爬升。 */
	virt_now = 200;
	zassert_equal(entelea_safety_force_channel_fault("ramp"), ENTELEA_OK);
	virt_now = 210;
	zassert_equal(entelea_safety_channel_recover("ramp"), ENTELEA_OK); /* 0@210 */
	virt_now = 211; /* 界 = 5×1 = 5 */
	zassert_equal(entelea_safety_commit("ramp", (entelea_out_value_t){.u = 100}),
		      ENTELEA_E_RANGE);
	zassert_equal(entelea_safety_readback("ramp", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 5, "recover 后从确认值爬升");
}

/* ---- estop 锁存不被 poweron_init 覆盖（缺陷 3 守卫） ---- */
ZTEST(framework_safety, test_poweron_preserves_estop_latch)
{
	entelea_safety_test_reset();
	zassert_equal(entelea_safety_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 0;

	static const entelea_out_channel_t pc = {
		.uid = "estop-guard", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = true}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	zassert_equal(entelea_safety_register_channel(&pc), ENTELEA_OK);
	zassert_equal(entelea_safety_estop_init(), ENTELEA_OK);
	entelea_safety_force_all_fault();
	entelea_ch_state_t st;
	entelea_out_value_t rb;

	zassert_equal(entelea_safety_channel_state("estop-guard", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT);

	entelea_write_rec_t recs[8];
	size_t before = entelea_driver_sim_writes("estop-guard", recs, 8);
	zassert_equal(entelea_safety_poweron_init(), ENTELEA_OK);
	zassert_equal(entelea_safety_channel_state("estop-guard", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT, "poweron 不得覆盖 estop 锁存");
	zassert_equal(entelea_safety_readback("estop-guard", &rb), ENTELEA_OK);
	zassert_false(rb.b, "影子保持 fault 值");
	size_t after = entelea_driver_sim_writes("estop-guard", recs, 8);

	zassert_equal(after, before, "锁存期不产生新的物理写");
	/* 锁存期 set_link 同样不得改写该通道（SAFE_FAULT 非迁移源态）。 */
	zassert_equal(entelea_safety_set_link(false), ENTELEA_OK);
	zassert_equal(entelea_safety_channel_state("estop-guard", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT);
}

ZTEST(framework_safety, test_poweron_writes_driver)
{
	entelea_safety_test_reset();

	static const entelea_out_channel_t pc = {
		.uid = "pw-ch", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = true}, .linkloss = {.b = false},
		.fault = {.b = false},
	};

	zassert_equal(entelea_safety_register_channel(&pc), ENTELEA_OK);
	zassert_equal(entelea_safety_poweron_init(), ENTELEA_OK);

	entelea_write_rec_t w[4];
	size_t n = entelea_driver_sim_writes("pw-ch", w, 4);

	zassert_equal(n, 1, "poweron 落驱动 = 恰一笔物理写");
	zassert_equal(w[0].value_u, 1U, "写值 = 声明 poweron（GPIO true）");
	/* 无节点板（native_sim）estop 面空操作成功。 */
	zassert_equal(entelea_safety_estop_init(), ENTELEA_OK);
}
