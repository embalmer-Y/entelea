/* SPDX-License-Identifier: Apache-2.0 */
/* ts-periph L1/L2 测试（LLD-ts-periph §6）：注册职责链（缺三态/撞 uid/
 * safe.uid 不一致拒绝）/ ADC 只进输入侧 / DETACH→SAFE_FAULT / ATTACH 恢复。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/periph.h>
#include <ts/power.h>
#include <ts/safety.h>

static int attach_seen, detach_seen;
static char last_evt_uid[8];

static void on_periph_evt(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(u);
	const ts_periph_evt_t *pl = e->data;

	if (e->id == TS_EVT_PERIPH_ATTACH) {
		attach_seen++;
	} else if (e->id == TS_EVT_PERIPH_DETACH) {
		detach_seen++;
	}
	strncpy(last_evt_uid, pl->uid, sizeof(last_evt_uid) - 1);
}

ZTEST(framework_periph, test_01_register_chain)
{
	/* GPIO 描述符（完整三态） */
	static const ts_periph_desc_t gpio = {
		.uid = "pg0", .kind = TS_PK_GPIO, .dt_spec = NULL,
		.safe = {.uid = "pg0", .kind = TS_CH_GPIO,
			 .poweron = {.b = false}, .linkloss = {.b = false},
			 .fault = {.b = false}},
	};
	/* safe.uid 与 uid 不一致 → 拒绝（单源纪律） */
	static const ts_periph_desc_t bad_uid = {
		.uid = "pg1", .kind = TS_PK_GPIO,
		.safe = {.uid = "other", .kind = TS_CH_GPIO,
			 .poweron = {.b = false}, .linkloss = {.b = false},
			 .fault = {.b = false}},
	};
	/* 缺三态（fault 未赋初值之外——用 limit 违规构造拒绝路径） */
	static const ts_periph_desc_t power_ok = {
		.uid = "pp0", .kind = TS_PK_POWER,
		.safe = {.uid = "pp0", .kind = TS_CH_POWER,
			 .poweron = {.pwr = {false, 0}},
			 .linkloss = {.pwr = {false, 0}},
			 .fault = {.pwr = {false, 0}},
			 .limits = {.current_limit_ma = 120}},
	};
	static const ts_periph_desc_t adc = {
		.uid = "pa0", .kind = TS_PK_ADC,
	};

	ts_safety_test_reset();
	ts_periph_test_reset();
	ts_power_test_reset();
	zassert_equal(ts_periph_register(NULL), TS_E_PARAM);
	zassert_equal(ts_periph_register(&bad_uid), TS_E_PARAM, "safe.uid 不一致拒绝");
	zassert_equal(ts_periph_register(&gpio), TS_OK);
	zassert_equal(ts_periph_register(&gpio), TS_E_PARAM, "撞 uid 拒绝");
	zassert_equal(ts_periph_register(&power_ok), TS_OK, "POWER 走 ts-power 槽");
	zassert_equal(ts_periph_register(&adc), TS_OK);
	zassert_equal(ts_periph_count(), 3);

	/* 职责链落点：GPIO/POWER 有安全通道；ADC 无（DR-13 输入侧不进 safety） */
	ts_ch_state_t st;

	zassert_equal(ts_safety_channel_state("pg0", &st), TS_OK);
	zassert_equal(ts_safety_channel_state("pp0", &st), TS_OK);
	zassert_equal(ts_safety_channel_state("pa0", &st), TS_E_NOTFOUND,
		      "ADC 不进 ts-safety");
	/* ts-hal 实例面全类注册（寻址可达） */
	bool saw_adc = false, saw_pwr = false;

	for (size_t i = 0; i < ts_hal_dev_count(); i++) {
		const ts_hal_dev_desc_t *d = ts_hal_dev_get(i);

		if (d == NULL) continue;
		saw_adc = saw_adc || (strcmp(d->uid, "pa0") == 0 && d->kind == TS_DEV_ADC);
		saw_pwr = saw_pwr || (strcmp(d->uid, "pp0") == 0 && d->kind == TS_DEV_POWER);
	}
	zassert_true(saw_adc && saw_pwr, "ts-hal 实例寻址面齐备");

	/* POWER 槽语义：经 periph 注册的限流进了通道 limits（预算放大后隔离
	 * 限流路径：130 > 120 → commit 层 TS_E_RANGE） */
	ts_perm_table_t t;
	ts_ctx_t pctx;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("power:set:0", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&pctx, 3, &t), TS_OK);
	ts_power_test_set_budget(1000);
	ts_safety_set_link(true); /* 供电提交须 ACTIVE 态 */
	zassert_equal(ts_power_request(pctx, 0, true, 130), TS_E_RANGE,
		      "经 ts-power 槽继承限流（130 > 120）");
	zassert_equal(ts_power_request(pctx, 0, true, 100), TS_OK, "限内可达");
}

ZTEST(framework_periph, test_02_hotplug_detach_fault)
{
	static const ts_periph_desc_t gpio = {
		.uid = "hp0", .kind = TS_PK_GPIO,
		.safe = {.uid = "hp0", .kind = TS_CH_GPIO,
			 .poweron = {.b = false}, .linkloss = {.b = true},
			 .fault = {.b = false}},
	};

	ts_safety_test_reset();
	ts_periph_test_reset();
	ts_power_test_reset();
	zassert_equal(ts_periph_register(&gpio), TS_OK);
	ts_safety_set_link(true);
	attach_seen = detach_seen = 0;
	zassert_equal(ts_evt_subscribe(TS_EVT_PERIPH_DETACH, on_periph_evt, NULL), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_PERIPH_ATTACH, on_periph_evt, NULL), TS_OK);

	/* ACTIVE 下可写 */
	zassert_equal(ts_safety_commit("hp0", (ts_out_value_t){.b = true}), TS_OK);

	/* DETACH：通道进 SAFE_FAULT（物理不在场 = 输出拒绝）+ 事件 */
	zassert_equal(ts_periph_detach("hp0"), TS_OK);
	zassert_equal(detach_seen, 1);
	zassert_equal(strcmp(last_evt_uid, "hp0"), 0, "事件携带逻辑名");
	ts_ch_state_t st;

	zassert_equal(ts_safety_channel_state("hp0", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_FAULT, "DETACH → SAFE_FAULT");
	zassert_equal(ts_safety_commit("hp0", (ts_out_value_t){.b = true}), TS_E_STATE,
		      "不在场输出拒绝");

	/* ATTACH：恢复（链路已立 → ACTIVE，poweron 重放）+ 事件 */
	zassert_equal(ts_periph_attach("hp0"), TS_OK);
	zassert_equal(attach_seen, 1);
	zassert_equal(ts_safety_channel_state("hp0", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE, "重附后可再写");
	zassert_equal(ts_safety_commit("hp0", (ts_out_value_t){.b = true}), TS_OK);

	/* 未知 uid / ADC 类边界 */
	zassert_equal(ts_periph_detach("nope"), TS_E_NOTFOUND);
}

ZTEST(framework_periph, test_03_detach_during_estop_lock)
{
	static const ts_periph_desc_t gpio = {
		.uid = "hp1", .kind = TS_PK_GPIO,
		.safe = {.uid = "hp1", .kind = TS_CH_GPIO,
			 .poweron = {.b = false}, .linkloss = {.b = false},
			 .fault = {.b = false}},
	};

	ts_safety_test_reset();
	ts_periph_test_reset();
	ts_power_test_reset();
	zassert_equal(ts_periph_register(&gpio), TS_OK);
	/* estop 锁存期：recover 拒绝（单通道恢复不得越过全局锁存） */
	ts_safety_force_all_fault();
	zassert_equal(ts_periph_detach("hp1"), TS_OK); /* detach 本身仍可达 */
	zassert_equal(ts_periph_attach("hp1"), TS_E_STATE, "锁存期恢复拒绝");
	zassert_equal(ts_safety_clear_fault(), TS_OK); /* 测试直调清锁存 */
	zassert_equal(ts_periph_attach("hp1"), TS_OK, "锁存释放后恢复可达");
}

/* F-2（docs/history/review-archive）：栈上组装描述符注册的回归测试——注册源改为模块
 * 静态表后，调用帧返回，safety 通道引用（uid 查表/状态/写路径）仍须有效。
 * 修复前：注册的是调用方 &d->safe，帧返回即悬垂。 */
static ts_res_t register_from_stack_frame(void)
{
	const ts_periph_desc_t d = {
		.uid = "stk0", .kind = TS_PK_GPIO,
		.safe = {.uid = "stk0", .kind = TS_CH_GPIO,
			 .poweron = {.b = false}, .linkloss = {.b = false},
			 .fault = {.b = false}},
	};

	return ts_periph_register(&d);
}

ZTEST(framework_periph, test_04_stack_descriptor_regression)
{
	ts_ch_state_t st;

	ts_safety_test_reset();
	ts_periph_test_reset();
	ts_power_test_reset();
	zassert_equal(register_from_stack_frame(), TS_OK);
	zassert_equal(ts_periph_count(), 1);

	/* 调用帧已返回：通道引用仍可解析、状态可读 */
	zassert_equal(ts_safety_channel_state("stk0", &st), TS_OK,
		      "栈上描述符注册后引用仍有效");
	zassert_equal(st, TS_ST_SAFE_POWERON);

	/* 写路径可达（唯一写路径经 safety；先建链置 ACTIVE） */
	ts_safety_set_link(true);
	zassert_equal(ts_safety_commit("stk0", (ts_out_value_t){.b = true}), TS_OK,
		      "commit 经静态表引用落驱动");
	ts_out_value_t rb;

	zassert_equal(ts_safety_readback("stk0", &rb), TS_OK);
	zassert_true(rb.b, "读回 = 已提交值");
}

ZTEST_SUITE(framework_periph, NULL, NULL, NULL, NULL, NULL);
