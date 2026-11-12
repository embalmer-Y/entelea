/* SPDX-License-Identifier: Apache-2.0 */
/* R08 辅助闭环测试：有界窗/触发/候选权限门/保护层唯一路径/云停用。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/time.h>
#include <entelea/d12.h>
#include "d12_ports.h"

static entelea_result_t fake_read_ms(void *user, uint64_t *out)
{
	*out = *(uint64_t *)user; /* 测试时钟：显式推进 */
	return ENTELEA_OK;
}

static uint8_t d12_rec[ENTELEA_D12_REC_LEN];
static bool d12_rec_present;

static entelea_result_t d12_store_load(void *user,
				       uint8_t rec[ENTELEA_D12_REC_LEN])
{
	(void)user;
	if (!d12_rec_present) {
		return ENTELEA_E_NOTFOUND;
	}
	memcpy(rec, d12_rec, ENTELEA_D12_REC_LEN);
	return ENTELEA_OK;
}

static entelea_result_t d12_store_save(void *user,
				       const uint8_t rec[ENTELEA_D12_REC_LEN])
{
	(void)user;
	memcpy(d12_rec, rec, ENTELEA_D12_REC_LEN);
	d12_rec_present = true;
	return ENTELEA_OK;
}

static entelea_result_t d12_wall_read(void *user, uint64_t *out)
{
	*out = *(uint64_t *)user;
	return ENTELEA_OK;
}

/* r16/WP4a：d12 闸接入面——model_test_reset 不动 d12，各段 reset 后重绑；
 * 每段为新设备（清持久替身，墙钟置于 UTC 日 20000）。 */
static void bind_d12_fake(void)
{
	static uint64_t fake_now;
	static uint64_t fake_utc_s = 20000ULL * 86400ULL;
	static entelea_clock_t d12_clock;
	static entelea_lock_t d12_lock;
	static entelea_wallclock_t d12_wall = {
		.read_utc_s = d12_wall_read,
	};
	static entelea_d12_store_t d12_store = {
		.load = d12_store_load,
		.save = d12_store_save,
	};

	fake_now = 0;
	d12_wall.user = &fake_utc_s;
	d12_rec_present = false;
	entelea_d12_test_reset();
	zassert_ok(entelea_clock_init(&d12_clock, fake_read_ms,
				      &fake_now, 0xD12));
	zassert_ok(entelea_d12_bind_clock(&d12_clock));
	zassert_ok(entelea_platform_d12_lock_init(&d12_lock));
	zassert_ok(entelea_d12_bind_lock(&d12_lock));
	zassert_ok(entelea_d12_bind_wallclock(&d12_wall));
	zassert_ok(entelea_d12_bind_store(&d12_store));
}

#include <entelea/assistant.h>
#include <entelea/capability.h>
#include <entelea/events.h>
#include <entelea/netmon.h>
#include <entelea/principal.h>
#include <entelea/model.h>
#include <entelea/resource.h>
#include <entelea/safety.h>
#include <entelea/task.h>

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

/* RF1 P1-2：执行门必须写**已裁决描述符**的 safety_uid，而非候选的逻辑
 * 名——逻辑名与他人保护通道同名时不得跨写，逻辑名≠通道名时合法目标
 * 必须可达；POWER 值只打 POWER 类描述符。 */
ZTEST(framework_assist, test_output_targets_checked_safety_uid)
{
	entelea_safety_test_reset();
	entelea_perm_test_reset();
	entelea_periph_test_reset();
	entelea_power_test_reset();
	entelea_power_test_set_budget(100);
	static const entelea_out_channel_t lamp_ch = {
		.uid = "lamp_ch", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	static const entelea_out_channel_t guarded_ch = {
		.uid = "lamp", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	static const entelea_out_channel_t pwr_ch = {
		.uid = "pwr_ch", .kind = ENTELEA_CH_POWER,
		.poweron = {.pwr = {false, 0}}, .linkloss = {.pwr = {false, 0}},
		.fault = {.pwr = {false, 0}},
		.limits = {.current_limit_ma = 1000},
	};

	zassert_equal(entelea_safety_register_channel(&lamp_ch), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&guarded_ch), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&pwr_ch), ENTELEA_OK);
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);
	static const entelea_periph_desc_t descs[] = {
		{.uid = "lamp", .kind = ENTELEA_DEV_OUTPUT, .inst = 0,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = "lamp_ch"},
		{.uid = "guarded", .kind = ENTELEA_DEV_OUTPUT, .inst = 5,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = "lamp"},
		{.uid = "heater", .kind = ENTELEA_DEV_OUTPUT, .inst = 0,
		 .perm_class = ENTELEA_PERM_POWER, .safety_uid = "pwr_ch"},
	};

	zassert_equal(entelea_periph_register(descs, 3), ENTELEA_OK);
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:write:0", &t), ENTELEA_OK);
	zassert_equal(entelea_perm_parse("power:write:0", &t), ENTELEA_OK);
	entelea_principal_t actor;

	zassert_equal(entelea_principal_bind(&actor, 21, &t), ENTELEA_OK);
	entelea_candidate_t cand = {
		.kind = ENTELEA_ACT_OUTPUT,
		.source = {.id = 1, .kind = ENTELEA_SOURCE_MODEL},
		.target_uid = "lamp", .value_kind = 0, .value_b = true,
	};
	entelea_out_value_t rb;

	zassert_equal(entelea_assistant_execute(actor, &cand, NULL), ENTELEA_OK);
	zassert_equal(entelea_safety_readback("lamp", &rb), ENTELEA_OK);
	zassert_false(rb.b, "无 gpio:write:5 不得写 guarded 的同名通道");
	zassert_equal(entelea_safety_readback("lamp_ch", &rb), ENTELEA_OK);
	zassert_true(rb.b, "落值到已裁决描述符的 safety_uid");

	cand.target_uid = "heater";
	cand.value_kind = 2;
	cand.value_u = 50;
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL), ENTELEA_OK,
		      "POWER 逻辑名≠通道名仍可达");
	zassert_equal(entelea_safety_readback("pwr_ch", &rb), ENTELEA_OK);
	zassert_true(rb.pwr.en && rb.pwr.ma == 50);

	cand.target_uid = "lamp";
	cand.value_b = false;
	zassert_equal(entelea_assistant_execute(actor, &cand, NULL),
		      ENTELEA_E_PARAM, "POWER 值打 GPIO 类描述符拒");
	zassert_equal(entelea_safety_readback("lamp_ch", &rb), ENTELEA_OK);
	zassert_true(rb.b, "跨类拒零输出");
}


/* ---- B-2 模型适配层：依赖联动/有界重试/用量累计/参数域 ---- */
static uint32_t mock_calls;
static entelea_result_t mock_next_rc[8];
static uint32_t mock_tokens_per_call;
static char mock_answer[32];

static entelea_result_t mock_chat(void *user,
				  const entelea_model_msg_t *msgs, uint8_t n,
				  char *out, size_t out_max,
				  uint32_t timeout_ms,
				  entelea_model_usage_t *usage)
{
	ARG_UNUSED(user);
	ARG_UNUSED(msgs);
	ARG_UNUSED(n);
	ARG_UNUSED(timeout_ms);
	if (mock_calls < 8) {
		entelea_result_t r = mock_next_rc[mock_calls];

		mock_calls++;
		if (usage != NULL) {
			/* 失败尝试也报用量（合同：成本如实累计）。 */
			usage->prompt_tokens = mock_tokens_per_call;
			usage->completion_tokens = mock_tokens_per_call;
		}
		if (r == ENTELEA_OK) {
			strncpy(out, mock_answer, out_max - 1);
			out[out_max - 1] = '\0';
		}
		return r;
	}
	return ENTELEA_E_IO;
}

static const entelea_model_transport_t mock_transport = {.chat = mock_chat};

static void deps_up(void)
{
	/* 防抖 2 连 good（R06 合同）。 */
	for (int i = 0; i < 2; i++) {
		zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, true, 600),
			      ENTELEA_OK);
		zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 600),
			      ENTELEA_OK);
	}
}

ZTEST(framework_assist, test_unknown_io_is_not_retried)
{
	char out[128] = "stale";
	entelea_model_msg_t msgs[1] = {
		{.role = "user", .content = "ping"}};
	entelea_model_usage_t usage;

	entelea_model_test_reset();
	entelea_net_test_reset();
	bind_d12_fake();
	entelea_model_bind(&mock_transport, NULL);
	deps_up();
	mock_calls = 0;
	memset(mock_next_rc, 0, sizeof(mock_next_rc));
	mock_next_rc[0] = ENTELEA_E_IO;
	mock_next_rc[1] = ENTELEA_OK;
	mock_tokens_per_call = 1;
	strcpy(mock_answer, "pong");
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &usage),
		      ENTELEA_E_IO, "未知发送效果不能自动重发");
	zassert_equal(mock_calls, 1, "失败后不得再次触及传输");
	zassert_equal(entelea_d12_model_calls(), 1);
	zassert_equal(out[0], '\0', "失败不得保留旧回答");
	entelea_model_test_reset();
	entelea_net_test_reset();
}

ZTEST(framework_assist, test_model_adapter_policy)
{
	char out[128];
	entelea_model_msg_t msgs[1] = {
		{.role = "user", .content = "ping"}};
	entelea_model_usage_t u;

	entelea_model_test_reset();
	entelea_net_test_reset();
	bind_d12_fake();
	mock_calls = 0;
	memset(mock_next_rc, 0, sizeof(mock_next_rc));
	strcpy(mock_answer, "pong");
	mock_tokens_per_call = 10;

	/* 未绑传输：E_UNINIT；参数域：out 过小 E_RANGE。 */
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_E_UNINIT);
	entelea_model_bind(&mock_transport, NULL);
	zassert_equal(entelea_model_ask(msgs, 1, out, 16, 5000, &u),
		      ENTELEA_E_RANGE, "推理模型输出面下限拒");

	/* 依赖 DOWN：E_STATE 且零调用（联动门先于传输）。 */
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_E_STATE);
	zassert_equal(mock_calls, 0, "依赖门不触传输");
	deps_up();
	zassert_true(entelea_model_ready());

	/* 仅明确未发送可重试：3 次失败达到封顶。 */
	mock_calls = 0;
	mock_next_rc[0] = mock_next_rc[1] = mock_next_rc[2] = ENTELEA_E_NOT_SENT;
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_E_NOT_SENT);
	zassert_equal(mock_calls, 3, "首发+2 重试封顶");
	zassert_equal(entelea_model_usage_total().prompt_tokens, 30,
		      "失败尝试用量如实累计（3x10，review P3-4 激活）");
	/* RF1 G5（审计 #11）：每次传输尝试=一次 D12 请求（逐尝试过闸+
	 * 逐尝试上报）——3 次失败即 3 连败闩风暴。 */
	zassert_equal(entelea_d12_model_calls(), 3, "逐尝试计入频度");
	zassert_true(entelea_d12_storm_latched(), "3 次传输失败闩风暴");
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_DOWN);
	mock_calls = 0;
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_E_STATE, "风暴闩内拒");
	zassert_equal(mock_calls, 0, "闩内不触传输");
	entelea_d12_storm_clear(); /* 外部释放面（命令/到场） */
	deps_up(); /* 此处为测试夹具；真实恢复须两次实际成功探测。 */

	/* 第 2 次成功：恰一次重试。 */
	mock_calls = 0;
	mock_next_rc[0] = ENTELEA_E_NOT_SENT;
	mock_next_rc[1] = ENTELEA_OK;
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_OK);
	zassert_equal(mock_calls, 2);
	zassert_equal(strcmp(out, "pong"), 0);
	zassert_equal(entelea_model_usage_total().prompt_tokens, 50,
		      "累计=3 失败+1 成功+1 成功（连续累计）");
	zassert_equal(entelea_model_usage_total().completion_tokens, 50);

	/* E_TIMEOUT 不自动重试（成本不可知——显式上抛）。 */
	entelea_model_test_reset();
	bind_d12_fake();
	entelea_model_bind(&mock_transport, NULL); /* reset 解绑——重绑 */
	mock_calls = 0;
	memset(mock_next_rc, 0, sizeof(mock_next_rc));
	mock_next_rc[0] = ENTELEA_E_TIMEOUT;
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_E_TIMEOUT);
	zassert_equal(mock_calls, 1, "超时不重试");

	/* 依赖再 DOWN：回到联动门。 */
	bind_d12_fake();
	entelea_dep_report(ENTELEA_DEP_NET, false, 0);
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_E_STATE);
	entelea_model_test_reset();
	entelea_net_test_reset();
}

/* RF1 G5（审计 #11）：重试同受频度闸——窗满即停，返回闸结果且不再
 * 触传输（旧实现仅首发过闸，6 次 ask 实发 18 次请求）。 */
ZTEST(framework_assist, test_model_retry_gated_by_d12)
{
	char out[128];
	entelea_model_msg_t msgs[1] = {
		{.role = "user", .content = "ping"}};
	entelea_model_usage_t u;

	entelea_model_test_reset();
	entelea_net_test_reset();
	bind_d12_fake();
	entelea_model_bind(&mock_transport, NULL);
	deps_up();
	memset(mock_next_rc, 0, sizeof(mock_next_rc));
	strcpy(mock_answer, "pong");
	mock_tokens_per_call = 1;
	mock_calls = 0;
	for (int i = 0; i < 5; i++) {
		zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out),
						5000, &u), ENTELEA_OK);
	}
	zassert_equal(entelea_d12_model_calls(), 5);
	/* 第 6 槽首发未发送；重试被窗拒 → E_BUSY，传输仅 1 次。 */
	mock_calls = 0;
	mock_next_rc[0] = mock_next_rc[1] = mock_next_rc[2] = ENTELEA_E_NOT_SENT;
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, &u),
		      ENTELEA_E_BUSY, "重试过闸被拒");
	zassert_equal(mock_calls, 1, "被闸重试不触传输");
	zassert_equal(out[0], '\0', "失败路径不留半答");
	zassert_equal(entelea_d12_model_calls(), 6);
	zassert_false(entelea_d12_storm_latched(), "闸拒不算传输失败");
	entelea_model_test_reset();
	entelea_net_test_reset();
}

ZTEST(framework_assist, test_model_probe_and_real_recovery_observations)
{
	char out[128];
	entelea_model_msg_t msgs[1] = {
		{.role = "user", .content = "ping"}};
	const entelea_model_transport_t invalid = {.chat = NULL};

	entelea_model_test_reset();
	entelea_net_test_reset();
	bind_d12_fake();
	entelea_model_bind(&invalid, NULL);
	zassert_equal(entelea_model_probe(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_E_UNINIT, "空回调不可解引用");
	entelea_model_bind(&mock_transport, NULL);
	mock_calls = 0;
	memset(mock_next_rc, 0, sizeof(mock_next_rc));
	strcpy(mock_answer, "pong");
	mock_tokens_per_call = 1;
	zassert_equal(entelea_model_probe(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_E_STATE, "网络未就绪不允许探测");
	zassert_equal(mock_calls, 0);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 1), ENTELEA_OK);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 2), ENTELEA_OK);
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_E_STATE, "模型未恢复时普通调用拒绝");
	zassert_equal(entelea_model_probe(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_DOWN,
		      "探测本身不伪造恢复观测");
	zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, true, 3), ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_DEGRADED);
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_E_STATE);
	zassert_equal(entelea_model_probe(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, true, 4), ENTELEA_OK);
	zassert_true(entelea_model_ready());
	zassert_equal(mock_calls, 2, "两条成功观测来自两次真实传输回调");
	zassert_equal(entelea_d12_model_calls(), 2);

	mock_next_rc[2] = ENTELEA_E_PERM;
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_E_PERM);
	zassert_equal(mock_calls, 3, "鉴权失败不重试");
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_DOWN);
	zassert_false(entelea_model_ready());
	entelea_model_test_reset();
	entelea_net_test_reset();
}

ZTEST(framework_assist, test_model_usage_saturates)
{
	char out[128];
	entelea_model_msg_t msgs[1] = {
		{.role = "user", .content = "ping"}};

	entelea_model_test_reset();
	entelea_net_test_reset();
	bind_d12_fake();
	entelea_model_bind(&mock_transport, NULL);
	deps_up();
	mock_calls = 0;
	memset(mock_next_rc, 0, sizeof(mock_next_rc));
	strcpy(mock_answer, "pong");
	mock_tokens_per_call = UINT32_MAX;
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_model_usage_total().prompt_tokens, UINT32_MAX);
	zassert_equal(entelea_model_usage_total().completion_tokens, UINT32_MAX);
	entelea_model_test_reset();
	entelea_net_test_reset();
}

/* ---- B-3 任务受理面：接线 R04 任务机+来源化反馈 ---- */
ZTEST(framework_assist, test_task_acceptance_wiring)
{
	entelea_task_test_reset();
	entelea_perm_test_reset();
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:read:0", &t), ENTELEA_OK);
	entelea_principal_t actor;

	zassert_equal(entelea_principal_bind(&actor, 21, &t), ENTELEA_OK);
	entelea_candidate_t cand = {
		.kind = ENTELEA_ACT_TASK,
		.source = {.id = 77, .kind = ENTELEA_SOURCE_MODEL},
		.target_uid = "demo-task",
		.value_u = 0, /* 无截止 */
	};
	int32_t ret = -1;

	zassert_equal(entelea_assistant_execute(actor, &cand, &ret),
		      ENTELEA_OK);
	zassert_true(ret >= 1 && ret <= 4, "task_ret=任务 id");
	entelea_task_id_t id = (entelea_task_id_t)ret; /* 独立持有（执行
	 * 前导置 0 语义：失败调用后 ret 不可复用） */
	entelea_task_t view;

	zassert_equal(entelea_task_get(id, &view), ENTELEA_OK);
	zassert_equal(view.state, ENTELEA_TASK_ACCEPTED);
	zassert_equal(view.seq, 77, "来源回合序=受理序（幂等键）");
	/* 重复候选（同 seq 非终态）=E_BUSY——模型重放不静默双受理。 */
	zassert_equal(entelea_assistant_execute(actor, &cand, &ret),
		      ENTELEA_E_BUSY,
		      "同 seq 重复候选可检测（执行前导置 0 不复用旧 id）");
	/* 来源化反馈：task_get effect 域（取消=NONE 起点语义）。 */
	zassert_equal(entelea_task_cancel(actor, id), ENTELEA_OK);
	zassert_equal(entelea_task_get(id, &view), ENTELEA_OK);
	zassert_equal(view.effect, ENTELEA_EFFECT_NONE, "反馈面=effect 域");
	/* 无名任务拒。 */
	entelea_candidate_t anon = cand;

	anon.target_uid = NULL;
	zassert_equal(entelea_assistant_execute(actor, &anon, &ret),
		      ENTELEA_E_PARAM);
	/* deadline≠0 而时钟未绑=E_STATE 透传（受理门）。 */
	entelea_candidate_t dl = cand;

	dl.value_u = 12345;
	zassert_equal(entelea_assistant_execute(actor, &dl, &ret),
		      ENTELEA_E_STATE, "deadline 不可知不受理（透传）");
	entelea_task_test_reset();
	entelea_perm_test_reset();
}
