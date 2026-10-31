/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ts-safety L1 测试（LLD-ts-safety §8）。
 * 说明：本套件为**有状态顺序场景**（注册表/forced 锁存为全局静态，estop 是终态），
 * ztest 用例执行顺序不保证 → 合并为单一顺序场景用例，断言链 = 规格链。
 */
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>
#include <stdio.h>
#include <ts/core.h>
#include <ts/safety.h>

static uint64_t virt_now;
static uint64_t virt_now_fn(void)
{
	return virt_now;
}
static const ts_time_source_t virt_src = {.now_ms = virt_now_fn};

static ts_res_t isr_commit_res;

static void isr_commit_fn(const void *arg)
{
	const char *uid = arg;
	ts_out_value_t v = {.u = 500};

	isr_commit_res = ts_safety_commit(uid, v);
}

ZTEST(framework_safety, test_safety_full_scenario)
{
	ts_out_value_t v = {.u = 0};
	ts_out_value_t rb = {0};
	ts_ch_state_t st;

	ts_safety_test_reset(); /* 容量算术敏感（fill 至 32）——显式净基线 */
	ts_time_test_bind(&virt_src);
	virt_now = 0;

	/* ---- 1. 注册校验（合同 1：三态缺一/值越限/uid 非法 → 拒绝，不留半注册） ---- */
	zassert_equal(ts_safety_register_channel(NULL), TS_E_PARAM);
	const ts_out_ch_t over_max = {
		.uid = "bad1", .kind = TS_CH_PWM,
		.poweron = {.u = 2000}, .linkloss = {.u = 0}, .fault = {.u = 0},
		.limits = {.min = 0, .max = 1000},
	};
	zassert_equal(ts_safety_register_channel(&over_max), TS_E_PARAM, "poweron 超 max 拒绝");

	static const ts_out_ch_t pwm_ch = { /* min=100 限幅用例 */
		.uid = "pwm1", .kind = TS_CH_PWM,
		.poweron = {.u = 100}, .linkloss = {.u = 100}, .fault = {.u = 100},
		.limits = {.min = 100, .max = 1000},
	};
	static const ts_out_ch_t slew_ch = { /* slew 用例：5/ms */
		.uid = "slew1", .kind = TS_CH_PWM,
		.poweron = {.u = 0}, .linkloss = {.u = 0}, .fault = {.u = 0},
		.limits = {.min = 0, .max = 1000, .slew_per_ms = 5},
	};
	static const ts_out_ch_t pwr_ch = { /* 受控供电（合同 7）：限流 500mA */
		.uid = "pwr1", .kind = TS_CH_POWER,
		.poweron = {.pwr = {.en = false, .ma = 0}},
		.linkloss = {.pwr = {.en = false, .ma = 0}},
		.fault = {.pwr = {.en = false, .ma = 0}},
		.limits = {.current_limit_ma = 500},
	};
	zassert_equal(ts_safety_register_channel(&pwm_ch), TS_OK);
	zassert_equal(ts_safety_register_channel(&slew_ch), TS_OK);
	zassert_equal(ts_safety_register_channel(&pwr_ch), TS_OK);
	zassert_equal(ts_safety_register_channel(&pwm_ch), TS_E_PARAM, "uid 撞名拒绝");

	/* ---- 2. 状态机与写路径保护 ---- */
	/* 非 ACTIVE（SAFE_POWERON）写入被拒（合同 3：输出拒写，输入不受影响） */
	v.u = 500;
	zassert_equal(ts_safety_commit("pwm1", v), TS_E_STATE, "commit before link");

	ts_safety_set_link(true); /* 链路确立 → ACTIVE */
	zassert_equal(ts_safety_channel_state("pwm1", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE, "POWERON -> ACTIVE");

	/* 限幅：commit 20 → clamp min=100，res=TS_E_RANGE（err.h：限幅=拦截） */
	v.u = 20;
	zassert_equal(ts_safety_commit("pwm1", v), TS_E_RANGE);
	zassert_equal(ts_safety_readback("pwm1", &rb), TS_OK);
	zassert_equal(rb.u, 100, "clamped to min");

	/* 断链 → SAFE_LINKLOSS：写入拒 + shadow=安全值；恢复不自动回写（DR-04） */
	ts_safety_set_link(false);
	zassert_equal(ts_safety_channel_state("pwm1", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_LINKLOSS);
	v.u = 500;
	zassert_equal(ts_safety_commit("pwm1", v), TS_E_STATE, "commit in linkloss");
	zassert_equal(ts_safety_readback("pwm1", &rb), TS_OK);
	zassert_equal(rb.u, 100, "shadow = linkloss value");
	ts_safety_set_link(true);
	zassert_equal(ts_safety_readback("pwm1", &rb), TS_OK);
	zassert_equal(rb.u, 100, "DR-04: 恢复不回写断链前值");

	/* slew 数学（LLD §4-4）：slew1 5/ms */
	virt_now = 1000;
	v.u = 100;
	zassert_equal(ts_safety_commit("slew1", v), TS_OK);
	virt_now = 1010; /* Δt=10 → 界 = 5×11 = 55 */
	v.u = 155;
	zassert_equal(ts_safety_commit("slew1", v), TS_OK, "within slew bound");
	virt_now = 1011; /* Δt=1 → 界 = 5×2 = 10 */
	v.u = 300;
	zassert_equal(ts_safety_commit("slew1", v), TS_E_RANGE, "over slew → split");
	zassert_equal(ts_safety_readback("slew1", &rb), TS_OK);
	zassert_equal(rb.u, 165, "split to bound 155+10");

	/* ---- 3. 限流（TS_CH_POWER，LLD §4-5）：请求超限整笔拒绝 ---- */
	v.pwr.en = true;
	v.pwr.ma = 600;
	zassert_equal(ts_safety_commit("pwr1", v), TS_E_RANGE);
	zassert_equal(ts_safety_readback("pwr1", &rb), TS_OK);
	zassert_false(rb.pwr.en, "over-limit rejected entirely");
	v.pwr.ma = 400;
	zassert_equal(ts_safety_commit("pwr1", v), TS_OK);
	zassert_equal(ts_safety_readback("pwr1", &rb), TS_OK);
	zassert_true(rb.pwr.en);
	zassert_equal(rb.pwr.ma, 400);

	/* ---- 4. ISR 调 [thread] API 拒绝（LLD-ts-common §3） ---- */
	v.u = 500;
	zassert_equal(ts_safety_commit("pwm1", v), TS_OK, "前置：ACTIVE 可写");
	isr_commit_res = TS_OK;
	irq_offload(isr_commit_fn, "pwm1");
	zassert_equal(isr_commit_res, TS_E_PERM, "ISR 调 commit 必须被拒");

	/* ---- 5. 审计环形 64（DEC-27）：前段审计 9 次（ISR 拒绝发生在审计前，不计）
	 *        + 本段 70 次 = 79 → 保留 64，覆盖丢弃 15（DR-07） ---- */
	for (int i = 0; i < 70; i++) {
		ts_safety_commit("pwm1", v);
	}
	ts_audit_entry_t entries[64];
	size_t n = ts_safety_audit_copy(entries, 64);

	zassert_equal(n, 64, "ring keeps last 64");
	zassert_equal(ts_safety_audit_dropped(), 15, "9+70-64 overwritten");
	zassert_equal(entries[63].res, TS_OK, "newest is last commit");

	/* ---- 6. 容量 32（DEC-27）：填满后 NOMEM ---- */
	static ts_out_ch_t fill[29];
	static char names[29][16]; /* 静态存储：uid 必须指向持久内存 */

	for (int i = 0; i < 29; i++) {
		snprintf(names[i], sizeof(names[i]), "fill%02d", i);
		fill[i] = pwm_ch;
		fill[i].uid = names[i];
		zassert_equal(ts_safety_register_channel(&fill[i]), TS_OK, "fill %d", i);
	}
	static const ts_out_ch_t one_more = { /* 新 uid：撞满注册表 → NOMEM */
		.uid = "overflow1", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
	};
	zassert_equal(ts_safety_register_channel(&one_more), TS_E_NOMEM, "registry full");

	/* ---- 7. estop 直达 + 锁存与释放（合同 5/8；IR-01 后：clear 是唯一释放路径） ---- */
	ts_safety_force_all_fault();
	zassert_equal(ts_safety_channel_state("pwm1", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_FAULT);
	zassert_equal(ts_safety_commit("pwm1", v), TS_E_STATE, "commit locked after estop");
	zassert_equal(ts_safety_readback("pwm1", &rb), TS_OK);
	zassert_equal(rb.u, 100, "fault value applied");

	/* estop-clear（M1 直调 = 测试入口；运行期唯一入口 = sys:estop-clear，M3）：
	 * 释放锁存 → 条件恢复（link up → ACTIVE）→ 写路径恢复 */
	zassert_equal(ts_safety_clear_fault(), TS_OK);
	zassert_equal(ts_safety_channel_state("pwm1", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE, "conditional recovery to ACTIVE");
	v.u = 300;
	zassert_equal(ts_safety_commit("pwm1", v), TS_OK, "write path restored after clear");

	/* 二次 estop 可再次锁存（announce 复位，IR-01） */
	ts_safety_force_all_fault();
	zassert_equal(ts_safety_channel_state("pwm1", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_FAULT, "re-latch after clear");
	ts_time_test_bind(NULL);
}

ZTEST_SUITE(framework_safety, NULL, NULL, NULL, NULL, NULL);

/* ---- estop 边沿映射（DR-11 prov 化，单元 F）------------------------------ */
ZTEST(framework_safety, test_estop_edge_mapping)
{
	extern int ts_safety_estop_edge_of(uint8_t flags);

	zassert_equal(ts_safety_estop_edge_of(0), GPIO_INT_EDGE_RISING,
		      "缺省 = 上升（兼容既有）");
	zassert_equal(ts_safety_estop_edge_of(1), GPIO_INT_EDGE_FALLING);
	zassert_equal(ts_safety_estop_edge_of(2), GPIO_INT_EDGE_BOTH);
	zassert_equal(ts_safety_estop_edge_of(99), GPIO_INT_EDGE_RISING,
		      "未知 = fail-safe 上升");
}

/* ---- poweron 值落驱动（单元 G1）------------------------------------------- */
ZTEST(framework_safety, test_poweron_writes_driver)
{
	/* 注册 → poweron_init：物理写序列第一笔 = 声明的 poweron 值
	 * （板级九遗留收口——此前仅置 shadow）。 */
	ts_safety_test_reset(); /* 后序 full_scenario 要填容量 32——先清台 */

	static const ts_out_ch_t pc = {
		.uid = "pw-ch", .kind = TS_CH_GPIO,
		.poweron = {.b = true}, .linkloss = {.b = false},
		.fault = {.b = false},
	};

	zassert_equal(ts_safety_register_channel(&pc), TS_OK);
	zassert_equal(ts_safety_poweron_init(), TS_OK);

	ts_write_rec_t w[4];
	size_t n = ts_driversim_writes("pw-ch", w, 4);

	zassert_equal(n, 1, "poweron 落驱动 = 恰一笔物理写");
	zassert_equal(w[0].value_u, 1U, "写值 = 声明 poweron（GPIO true）");
}

/* ---- 控制源仲裁（CG3 · Q-41②/合同 11 · LLD §5b）------------------------ */

static ts_arb_evt_t arb_seen[4];
static int arb_seen_n;

static void arb_evt_fn(const ts_evt_t *evt, void *user)
{
	(void)user;
	if (evt->id == TS_EVT_ARB_CHANGED && evt->data != NULL && arb_seen_n < 4) {
		memcpy(&arb_seen[arb_seen_n++], evt->data, sizeof(ts_arb_evt_t));
	}
}

ZTEST(framework_safety, test_arb_seat_and_suppression)
{
	static bool hooked;

	if (!hooked) {
		zassert_equal(ts_evt_subscribe(TS_EVT_ARB_CHANGED, arb_evt_fn, NULL), TS_OK);
		hooked = true;
	}
	arb_seen_n = 0;
	ts_safety_test_reset();

	static const ts_out_ch_t ac = {
		.uid = "arb-ch", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = true}, .fault = {.b = false},
	};
	ts_ch_state_t st;
	ts_out_value_t rb;

	zassert_equal(ts_safety_register_channel(&ac), TS_OK);
	zassert_equal(ts_safety_poweron_init(), TS_OK);
	ts_safety_set_link(true);
	zassert_equal(ts_safety_commit("arb-ch", (ts_out_value_t){.b = true}), TS_OK);
	ts_write_rec_t w0[8];
	size_t n0 = ts_driversim_writes("arb-ch", w0, 8);

	/* 席位变更：零输出（无新物理写/状态影子不变）+ 事件 old/owner */
	zassert_equal(ts_safety_set_arb(TS_ARB_LOCAL), TS_OK);
	zassert_equal(arb_seen_n, 1, "变更产一条事件");
	zassert_equal(arb_seen[0].owner, TS_ARB_LOCAL);
	zassert_equal(arb_seen[0].old, TS_ARB_REMOTE);
	zassert_equal(ts_safety_channel_state("arb-ch", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE, "席位变更零状态迁移");
	zassert_equal(ts_safety_readback("arb-ch", &rb), TS_OK);
	zassert_true(rb.b, "席位变更零输出（影子不变）");
	ts_write_rec_t w1[8];
	size_t n1 = ts_driversim_writes("arb-ch", w1, 8);
	zassert_equal(n1, n0, "席位变更无新物理写（EN 60204-1 §9.2.3）");

	/* 压制：ZENOH 来源拒（审计留痕 src 维度）；APP 路径放行 */
	zassert_equal(ts_safety_commit_src("arb-ch", (ts_out_value_t){.b = false},
					   TS_SRC_ZENOH),
		      TS_E_STATE, "本地激活压制远程编排写");
	ts_audit_entry_t ae[8];
	size_t an = ts_safety_audit_copy(ae, 8);

	zassert_true(an >= 2);
	zassert_equal(ae[0].res, TS_E_STATE);
	zassert_equal(ae[0].src, TS_SRC_ZENOH, "审计来源维度（压制归因）");
	zassert_equal(ts_safety_commit("arb-ch", (ts_out_value_t){.b = false}), TS_OK,
		      "APP 路径不经仲裁闸（编排意图载体）");
	an = ts_safety_audit_copy(ae, 8);
	zassert_equal(ae[0].src, TS_SRC_APP);

	/* 幂等变更：零事件零输出 */
	zassert_equal(ts_safety_set_arb(TS_ARB_LOCAL), TS_OK);
	zassert_equal(arb_seen_n, 1, "幂等变更零事件");

	/* 释放：远程来源恢复 */
	zassert_equal(ts_safety_set_arb(TS_ARB_REMOTE), TS_OK);
	zassert_equal(arb_seen_n, 2);
	zassert_equal(arb_seen[1].owner, TS_ARB_REMOTE);
	zassert_equal(arb_seen[1].old, TS_ARB_LOCAL);
	zassert_equal(ts_safety_commit_src("arb-ch", (ts_out_value_t){.b = true},
					   TS_SRC_ZENOH),
		      TS_OK, "释放后远程写恢复");

	ts_safety_test_reset(); /* 后序用例干净 */
}
