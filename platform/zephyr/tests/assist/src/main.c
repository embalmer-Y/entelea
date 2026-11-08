/* SPDX-License-Identifier: Apache-2.0 */
/* R08 辅助闭环测试：有界窗/触发/候选权限门/保护层唯一路径/云停用。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/assistant.h>
#include <entelea/capability.h>
#include <entelea/events.h>
#include <entelea/netmon.h>
#include <entelea/principal.h>
#include <entelea/resource.h>
#include <entelea/safety.h>

static void *assist_setup(void)
{
	entelea_safety_test_reset();
	entelea_perm_test_reset();
	entelea_net_test_reset();
	return NULL;
}

ZTEST(framework_assist, test_context_window_bounds)
{
	entelea_context_window_t w;

	memset(&w, 0, sizeof(w));
	entelea_ctx_event_t e = {.id = ENTELEA_EVT_WDT_WARN, .t_ms = 1};

	for (int i = 0; i < ENTELEA_CTX_EVENTS_MAX + 5; i++) {
		e.t_ms = (uint32_t)i;
		entelea_ctx_push(&w, &e);
	}
	zassert_equal(w.count, ENTELEA_CTX_EVENTS_MAX, "有界");
	zassert_equal(w.dropped, 5, "溢出计数");
	entelea_ctx_event_t snap[ENTELEA_CTX_EVENTS_MAX];
	size_t n = 0;

	entelea_ctx_snapshot(&w, snap, sizeof(snap) / sizeof(snap[0]), &n);
	zassert_equal(n, ENTELEA_CTX_EVENTS_MAX);
	zassert_equal(snap[0].t_ms, 5, "最旧被弃（保留最近）");
	zassert_equal(snap[n - 1].t_ms, ENTELEA_CTX_EVENTS_MAX + 4);
	zassert_equal(entelea_ctx_dropped(&w), 5);
}

ZTEST(framework_assist, test_candidate_gate_and_protection)
{
	static const entelea_out_channel_t led = {
		.uid = "aled", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	zassert_equal(entelea_safety_register_channel(&led), ENTELEA_OK);
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);
	/* periph 注册（review P1 修复后 execute 经 R04 acquire 门）。 */
	static const entelea_periph_desc_t led_desc = {
		.uid = "aled", .kind = ENTELEA_DEV_OUTPUT, .inst = 0,
		.perm_class = ENTELEA_PERM_GPIO, .safety_uid = "aled",
	};

	zassert_equal(entelea_periph_register(&led_desc, 1), ENTELEA_OK);

	entelea_candidate_t cand = {
		.kind = ENTELEA_ACT_OUTPUT,
		.source = {.id = 1, .kind = ENTELEA_SOURCE_MODEL},
		.target_uid = "aled",
		.value_kind = 0,
		.value_b = true,
	};

	/* 未授权 principal → E_PERM（模型不能自批——经 acquire 权限门）。 */
	entelea_perm_table_t empty;

	entelea_perm_table_init(&empty);
	entelea_principal_t poor;

	zassert_equal(entelea_principal_bind(&poor, 9, &empty), ENTELEA_OK);
	zassert_equal(entelea_assistant_execute(poor, &cand, NULL),
		      ENTELEA_E_PERM);
	/* 未注册目标 → E_NOTFOUND（不静默成功）。 */
	entelea_candidate_t ghost = cand;

	ghost.target_uid = "no-such-uid";
	zassert_equal(entelea_assistant_execute(poor, &ghost, NULL),
		      ENTELEA_E_NOTFOUND);
	entelea_out_value_t rb;

	zassert_equal(entelea_safety_readback("aled", &rb), ENTELEA_OK);
	zassert_false(rb.b, "E_PERM 路径零输出");

	/* 授权 principal → 保护层唯一写路径（安全态拒/正常写）。 */
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:write:0", &t), ENTELEA_OK);
	entelea_principal_t actor;

	zassert_equal(entelea_principal_bind(&actor, 1, &t), ENTELEA_OK);
	entelea_safety_set_link(false);
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_E_STATE, "SAFE_LINKLOSS 期拒（保护层）");
	entelea_safety_set_link(true);
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_safety_readback("aled", &rb), ENTELEA_OK);
	zassert_true(rb.b, "授权写经保护层落值");

	/* estop 锁存期拒（保护层全链复用）。 */
	entelea_safety_force_all_fault();
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_E_STATE);
	/* NOTIFY/NONE 无副作用。 */
	entelea_candidate_t note = {.kind = ENTELEA_ACT_NOTIFY};

	zassert_equal(entelea_assistant_execute(actor, &note, NULL), ENTELEA_OK);
	/* NULL/域外拒。 */
	zassert_equal(entelea_assistant_execute(actor, NULL, NULL),
		      ENTELEA_E_PARAM);
	entelea_candidate_t bad = cand;

	bad.kind = (entelea_action_kind_t)99;
	zassert_equal(entelea_assistant_execute(actor, &bad, NULL),
		      ENTELEA_E_PARAM);
}

ZTEST(framework_assist, test_cloud_down_stops_brain_only)
{
	/* 初始 DOWN → 停用智能。 */
	zassert_false(entelea_assistant_available());
	/* 网络恢复但模型仍 down → 仍停。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 100), ENTELEA_OK);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 200), ENTELEA_OK);
	zassert_false(entelea_assistant_available(), "model down → 停智能");
	/* 模型也恢复 → 可用。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, true, 300),
		      ENTELEA_OK);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, true, 400),
		      ENTELEA_OK);
	zassert_true(entelea_assistant_available());
	/* 网络再断 → 停（云模型经网络到达）。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, false, 500),
		      ENTELEA_OK);
	zassert_false(entelea_assistant_available());
	/* 独立 App 照常：storage 依赖独立判定（不绑死智脑）。 */
	zassert_equal(entelea_dep_state(ENTELEA_DEP_STORAGE), ENTELEA_DEP_DOWN);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_STORAGE, true, 600),
		      ENTELEA_OK);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_STORAGE, true, 700),
		      ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_STORAGE), ENTELEA_DEP_UP);
	zassert_false(entelea_assistant_available(),
		      "独立 App 依赖 UP 不改变智脑停用");
}

/* 触发：窗满判定（纯函数）。 */
ZTEST(framework_assist, test_trigger_on_full_window)
{
	entelea_context_window_t w;

	memset(&w, 0, sizeof(w));
	zassert_false(entelea_should_deliberate(&w, 0, NULL));
	entelea_ctx_event_t e = {0};

	for (int i = 0; i < ENTELEA_CTX_EVENTS_MAX - 1; i++) {
		entelea_ctx_push(&w, &e);
	}
	zassert_false(entelea_should_deliberate(&w, 0, NULL), "差一条");
	entelea_ctx_push(&w, &e);
	zassert_true(entelea_should_deliberate(&w, 0, NULL), "窗满触发");
}

ZTEST_SUITE(framework_assist, NULL, assist_setup, NULL, NULL, NULL);

/* ---- 输出域门（计划符合性审查 P2）：跨类/域外/POWER 路由/非输出目标 ---- */
ZTEST(framework_assist, test_output_domain_gates)
{
	/* 自含净基线（用例序不保证：清安全注册表/estop 锁存/principal 槽）。 */
	entelea_safety_test_reset();
	entelea_perm_test_reset();
	/* 注册 PWM 输出 + GPIO 输出 + 传感器（非输出）目标。 */
	static const entelea_out_channel_t pwmch = {
		.uid = "apwm", .kind = ENTELEA_CH_PWM,
		.poweron = {.u = 0}, .linkloss = {.u = 0}, .fault = {.u = 0},
	};
	static const entelea_out_channel_t gpioch = {
		.uid = "agio", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};

	zassert_equal(entelea_safety_register_channel(&pwmch), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&gpioch), ENTELEA_OK);
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);
	static const entelea_periph_desc_t descs[] = {
		{.uid = "apwm", .kind = ENTELEA_DEV_OUTPUT, .inst = 0,
		 .perm_class = ENTELEA_PERM_PWM, .safety_uid = "apwm"},
		{.uid = "agio", .kind = ENTELEA_DEV_OUTPUT, .inst = 0,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = "agio"},
		{.uid = "asens", .kind = ENTELEA_DEV_SENSOR, .inst = 0,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = NULL},
		{.uid = "apwr", .kind = ENTELEA_DEV_OUTPUT, .inst = 0,
		 .perm_class = ENTELEA_PERM_POWER, .safety_uid = "apwr"},
	};

	zassert_equal(entelea_periph_register(descs, 4), ENTELEA_OK);

	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:write:0", &t), ENTELEA_OK);
	zassert_equal(entelea_perm_parse("pwm:write:0", &t), ENTELEA_OK);
	entelea_principal_t actor;

	zassert_equal(entelea_principal_bind(&actor, 3, &t), ENTELEA_OK);

	entelea_candidate_t cand = {
		.kind = ENTELEA_ACT_OUTPUT,
		.source = {.id = 1, .kind = ENTELEA_SOURCE_MODEL},
		.value_b = true, .value_u = 100,
	};

	/* 跨类拒（双向）：GPIO 值打 PWM 目标 / PWM 值打 GPIO 目标。 */
	cand.target_uid = "apwm";
	cand.value_kind = 0;
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_E_PARAM, "GPIO 值打 PWM 目标拒");
	cand.target_uid = "agio";
	cand.value_kind = 1;
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_E_PARAM, "PWM 值打 GPIO 目标拒");
	entelea_out_value_t rb;

	zassert_equal(entelea_safety_readback("apwm", &rb), ENTELEA_OK);
	zassert_equal(rb.u, 0, "跨类拒零输出");
	zassert_equal(entelea_safety_readback("agio", &rb), ENTELEA_OK);
	zassert_false(rb.b, "跨类拒零输出（gpio）");

	/* 同类对照：正面路径仍通（防过度收紧）。 */
	cand.target_uid = "agio";
	cand.value_kind = 0;
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_OK, "同类（GPIO→GPIO）放行");

	/* 域外 value_kind 拒（>2）。 */
	cand.value_kind = 3;
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_E_PARAM, "value_kind 域外拒");

	/* 非 DEV_OUTPUT 目标拒：传感器描述符（READ 即可 acquire——但输出
	 * 动作必须打 DEV_OUTPUT；修复前跳过类检直落 safety_commit）。 */
	entelea_perm_table_t rt;

	entelea_perm_table_init(&rt);
	zassert_equal(entelea_perm_parse("gpio:read:0", &rt), ENTELEA_OK);
	entelea_principal_t reader;

	zassert_equal(entelea_principal_bind(&reader, 4, &rt), ENTELEA_OK);
	cand.target_uid = "asens";
	cand.value_kind = 0;
	zassert_equal(entelea_assistant_execute(reader, &cand, NULL),
		      ENTELEA_E_PARAM, "输出动作打非 DEV_OUTPUT 目标拒");

	/* POWER 路由（value_kind=2 必经 power_request）：无 power:write →
	 * E_PERM（不旁路预算门）。 */
	static const entelea_out_channel_t pwrch = {
		.uid = "apwr", .kind = ENTELEA_CH_POWER,
		.poweron = {.pwr = {false, 0}}, .linkloss = {.pwr = {false, 0}},
		.fault = {.pwr = {false, 0}},
		.limits = {.current_limit_ma = 1000},
	};

	zassert_equal(entelea_safety_register_channel(&pwrch), ENTELEA_OK);
	/* 注册晚于 set_link 边沿：翻转一次使新通道进入 ACTIVE。 */
	entelea_safety_set_link(false);
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);
	cand.target_uid = "apwr";
	cand.value_kind = 2;
	cand.value_b = true;
	cand.value_u = 50;
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_E_PERM, "无 power:write → power_request 权限门拒");
	zassert_equal(entelea_safety_readback("apwr", &rb), ENTELEA_OK);
	zassert_false(rb.b, "POWER 旁路拒：零输出");

	/* 有 power:write + 预算内 → 经 power_request 落值（正面对照）。 */
	entelea_power_test_reset();
	entelea_power_test_set_budget(100);
	entelea_perm_table_t pt;

	entelea_perm_table_init(&pt);
	zassert_equal(entelea_perm_parse("gpio:write:0", &pt), ENTELEA_OK);
	zassert_equal(entelea_perm_parse("power:write:0", &pt), ENTELEA_OK);
	entelea_principal_t pactor;

	zassert_equal(entelea_principal_bind(&pactor, 5, &pt), ENTELEA_OK);
	zassert_equal(entelea_assistant_execute(pactor, &cand, NULL),
		      ENTELEA_OK, "POWER 经 power_request 放行");
	zassert_equal(entelea_safety_readback("apwr", &rb), ENTELEA_OK);
	zassert_true(rb.b, "POWER 经预算门落值");
}
