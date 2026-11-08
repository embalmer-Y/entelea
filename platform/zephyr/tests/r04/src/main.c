/* SPDX-License-Identifier: Apache-2.0 */
/* R04 能力/权限/任务/资源测试（fault-class 矩阵：伪造句柄/越权/撞名/
 * 重复受理/过期取消/并发终态/预算竞争/半注册回滚/仲裁压制/非幂等重试面）。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/capability.h>
#include <entelea/events.h>
#include <entelea/principal.h>
#include <entelea/resource.h>
#include <entelea/safety.h>
#include <entelea/task.h>

/* 虚时钟（deadline 判定注入面：virt_now 前进/回退，fail_next 注入
 * 读失败——时间不可知路径）。 */
static uint64_t virt_now;
static bool clock_fail_next;
static bool clock_dead;

static entelea_result_t virt_read_ms(void *user, uint64_t *out)
{
	ARG_UNUSED(user);
	if (clock_dead) {
		return ENTELEA_E_IO;
	}
	if (clock_fail_next) {
		clock_fail_next = false;
		return ENTELEA_E_IO;
	}
	*out = virt_now;
	return ENTELEA_OK;
}

static const entelea_clock_t virt_clock = {
	.read_ms = virt_read_ms,
	.user = NULL,
	.clock_id = 0x7A4B,
	.last_ms = 0,
	.sampled = false,
};

static void *r04_setup(void)
{
	entelea_safety_test_reset();
	entelea_perm_test_reset();
	entelea_periph_test_reset();
	entelea_task_test_reset();
	entelea_power_test_reset();
	return NULL;
}

/* ---- principal：文法/绑定/伪造/过期 ---- */
ZTEST(framework_r04, test_principal_grammar_and_forgery)
{
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:write:0-3", &t), ENTELEA_OK);
	zassert_equal(entelea_perm_parse("pwm:write:1,3", &t), ENTELEA_OK);
	zassert_equal(entelea_perm_parse("msg:send:companion.x", &t), ENTELEA_OK);
	zassert_equal(entelea_perm_parse("msg:send:companion.x", &t), ENTELEA_OK,
		      "重复声明幂等");
	/* 域外/文法坏（fail-closed，不静默截断）。 */
	zassert_equal(entelea_perm_parse("bogus:write:0", &t), ENTELEA_E_PARAM);
	zassert_equal(entelea_perm_parse("gpio:bogus:0", &t), ENTELEA_E_PARAM);
	zassert_equal(entelea_perm_parse("gpio:write:32", &t), ENTELEA_E_PARAM);
	zassert_equal(entelea_perm_parse("gpio:write:3-1", &t), ENTELEA_E_PARAM);
	zassert_equal(entelea_perm_parse("msg:read:x", &t), ENTELEA_E_PARAM,
		      "msg 仅 send");
	zassert_equal(entelea_perm_parse("msg:send:bad name!", &t), ENTELEA_E_PARAM);

	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 7, &t), ENTELEA_OK);
	zassert_equal(entelea_perm_check(ctx, ENTELEA_PERM_GPIO,
					 ENTELEA_PERM_WRITE, 2),
		      ENTELEA_OK);
	zassert_equal(entelea_perm_check(ctx, ENTELEA_PERM_GPIO,
					 ENTELEA_PERM_WRITE, 4),
		      ENTELEA_E_PERM, "位图外实例拒");
	zassert_equal(entelea_perm_check(ctx, ENTELEA_PERM_POWER,
					 ENTELEA_PERM_WRITE, 0),
		      ENTELEA_E_PERM, "未声明类拒");
	zassert_equal(entelea_perm_check_msg(ctx, "companion.x"), ENTELEA_OK);
	zassert_equal(entelea_perm_check_msg(ctx, "other"), ENTELEA_E_PERM);

	/* 伪造句柄：域外/0/未绑定槽。 */
	entelea_principal_t forged = {._handle = 9, .app_id = 7};

	zassert_equal(entelea_perm_check(forged, ENTELEA_PERM_GPIO,
					   ENTELEA_PERM_WRITE, 0),
		      ENTELEA_E_PERM, "域外句柄拒");
	forged._handle = 0;
	zassert_equal(entelea_perm_check(forged, ENTELEA_PERM_GPIO,
					   ENTELEA_PERM_WRITE, 0),
		      ENTELEA_E_PERM, "零句柄拒");

	/* 过期（解绑后旧句柄）。 */
	entelea_principal_unbind(&ctx);
	zassert_equal(entelea_perm_check(ctx, ENTELEA_PERM_GPIO,
					 ENTELEA_PERM_WRITE, 0),
		      ENTELEA_E_PERM, "过期句柄拒");
	zassert_equal(entelea_perm_check_msg(ctx, "companion.x"), ENTELEA_E_PERM);

	/* app_id 撞名。 */
	entelea_principal_t a, b;

	zassert_equal(entelea_principal_bind(&a, 9, &t), ENTELEA_OK);
	zassert_equal(entelea_principal_bind(&b, 9, &t), ENTELEA_E_PARAM,
		      "撞名拒绝");
	entelea_principal_unbind(&a);
}

/* ---- 能力注册：原子回滚/acquire 门/detach ---- */
ZTEST(framework_r04, test_capability_registry_and_gates)
{
	static const entelea_out_channel_t led_ch = {
		.uid = "led0", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	zassert_equal(entelea_safety_register_channel(&led_ch), ENTELEA_OK);

	static const entelea_periph_desc_t good[] = {
		{.uid = "led0", .kind = ENTELEA_DEV_OUTPUT, .inst = 0,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = "led0"},
		{.uid = "btn0", .kind = ENTELEA_DEV_INPUT, .inst = 1,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = NULL},
	};
	zassert_equal(entelea_periph_register(good, 2), ENTELEA_OK);

	/* 半注册回滚：批内撞名 → 全部拒绝。 */
	static const entelea_periph_desc_t bad[] = {
		{.uid = "s1", .kind = ENTELEA_DEV_INPUT, .inst = 2,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = NULL},
		{.uid = "s1", .kind = ENTELEA_DEV_INPUT, .inst = 3,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = NULL},
	};
	zassert_equal(entelea_periph_register(bad, 2), ENTELEA_E_PARAM);
	zassert_equal(entelea_periph_find("s1"), NULL, "撞名批零注册");
	/* 输出无 safety uid 拒。 */
	static const entelea_periph_desc_t noout[] = {
		{.uid = "x1", .kind = ENTELEA_DEV_OUTPUT, .inst = 4,
		 .perm_class = ENTELEA_PERM_GPIO, .safety_uid = NULL},
	};
	zassert_equal(entelea_periph_register(noout, 1), ENTELEA_E_PARAM);

	/* acquire：授权 + ACTIVE 才放行。 */
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:write:0", &t), ENTELEA_OK);
	zassert_equal(entelea_perm_parse("gpio:read:1", &t), ENTELEA_OK,
		      "输入面 read 声明");
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 1, &t), ENTELEA_OK);
	const entelea_periph_desc_t *d = NULL;

	zassert_equal(entelea_periph_acquire(ctx, "led0", &d), ENTELEA_E_STATE,
		      "SAFE_POWERON 期不可提交");
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);
	zassert_equal(entelea_periph_acquire(ctx, "led0", &d), ENTELEA_OK);
	zassert_not_null(d);
	zassert_equal(entelea_periph_acquire(ctx, "btn0", &d), ENTELEA_OK,
		      "输入面 read 门");
	/* 无 gpio:write:1 的 principal 对 btn0（inst=1）越权。 */
	entelea_perm_table_t t2;

	entelea_perm_table_init(&t2);
	entelea_principal_t poor;

	zassert_equal(entelea_principal_bind(&poor, 2, &t2), ENTELEA_OK);
	zassert_equal(entelea_periph_acquire(poor, "btn0", &d), ENTELEA_E_PERM);
	/* detach → safety 单通道 fault（机制面复用）。 */
	zassert_equal(entelea_periph_detach("led0"), ENTELEA_OK);
	entelea_ch_state_t st;

	zassert_equal(entelea_safety_channel_state("led0", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT);
	zassert_equal(entelea_periph_acquire(ctx, "led0", &d), ENTELEA_E_STATE,
		      "detach 后 acquire 拒");
	entelea_principal_unbind(&ctx);
	entelea_principal_unbind(&poor);
}

/* ---- 任务状态机：重复/竞态/取消效果域/回收 ---- */
ZTEST(framework_r04, test_task_lifecycle_and_race_edges)
{
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 5, &t), ENTELEA_OK);
	entelea_task_id_t id = 0;

	zassert_equal(entelea_task_submit(ctx, "demo", 100, 0, &id), ENTELEA_OK);
	zassert_equal(id, 1);
	/* 重复受理（同 seq 非终态）= E_BUSY。 */
	zassert_equal(entelea_task_submit(ctx, "demo", 100, 0, &id), ENTELEA_E_BUSY);
	zassert_equal(entelea_task_start(ctx, id), ENTELEA_OK);
	zassert_equal(entelea_task_start(ctx, id), ENTELEA_E_STATE, "重复 start 拒");

	/* RUNNING 期取消 → 效果 UNKNOWN（不撤回已发生效果）。 */
	zassert_equal(entelea_task_cancel(ctx, id), ENTELEA_OK);
	entelea_task_t out;

	zassert_equal(entelea_task_get(id, &out), ENTELEA_OK);
	zassert_equal(out.state, ENTELEA_TASK_CANCELLED);
	zassert_equal(out.effect, ENTELEA_EFFECT_UNKNOWN);
	/* 终态后取消/成功 = E_STATE；回收后查询 NOTFOUND。 */
	zassert_equal(entelea_task_cancel(ctx, id), ENTELEA_E_STATE);
	zassert_equal(entelea_task_succeed(ctx, id, true), ENTELEA_E_STATE);
	zassert_equal(entelea_task_release(ctx, id), ENTELEA_OK);
	zassert_equal(entelea_task_get(id, &out), ENTELEA_E_NOTFOUND);

	/* ACCEPTED 期取消 → NONE；他者操作拒。 */
	entelea_task_id_t a = 0, b = 0;

	zassert_equal(entelea_task_submit(ctx, "t2", 101, 0, &a), ENTELEA_OK);
	entelea_principal_t other;

	zassert_equal(entelea_principal_bind(&other, 6, &t), ENTELEA_OK);
	zassert_equal(entelea_task_cancel(other, a), ENTELEA_E_PERM, "他者拒");
	zassert_equal(entelea_task_cancel(ctx, a), ENTELEA_OK);
	zassert_equal(entelea_task_get(a, &out), ENTELEA_OK);
	zassert_equal(out.effect, ENTELEA_EFFECT_NONE);

	/* 成功回执 effect 域由调用方给出；失败 unknown 如实呈现。 */
	zassert_equal(entelea_task_submit(ctx, "t3", 102, 0, &b), ENTELEA_OK);
	zassert_equal(entelea_task_start(ctx, b), ENTELEA_OK);
	zassert_equal(entelea_task_fail(ctx, b, true), ENTELEA_OK);
	zassert_equal(entelea_task_get(b, &out), ENTELEA_OK);
	zassert_equal(out.effect, ENTELEA_EFFECT_UNKNOWN,
		      "失败点在副作用后=unknown（禁自动重试面）");
	/* 回收终态槽（t2/t3）后槽满（4 上限）。 */
	zassert_equal(entelea_task_release(ctx, a), ENTELEA_OK);
	zassert_equal(entelea_task_release(ctx, b), ENTELEA_OK);
	entelea_task_id_t ids[4];

	for (int i = 0; i < 4; i++) {
		zassert_equal(entelea_task_submit(ctx, "fill", (uint16_t)(200 + i),
						  0, &ids[i]),
			      ENTELEA_OK);
	}
	zassert_equal(entelea_task_submit(ctx, "over", 999, 0, &id), ENTELEA_E_NOMEM);
	entelea_principal_unbind(&ctx);
	entelea_principal_unbind(&other);
}

/* ---- 资源预算：检查-提交原子化/超预算/降额不在线 ---- */
ZTEST(framework_r04, test_power_budget_atomic_request)
{
	static const entelea_out_channel_t pwr_a = {
		.uid = "pwra", .kind = ENTELEA_CH_POWER,
		.poweron = {.pwr = {.en = false, .ma = 0}},
		.linkloss = {.pwr = {.en = false, .ma = 0}},
		.fault = {.pwr = {.en = false, .ma = 0}},
		.limits = {.current_limit_ma = 1000},
	};
	static const entelea_out_channel_t pwr_b = {
		.uid = "pwrb", .kind = ENTELEA_CH_POWER,
		.poweron = {.pwr = {.en = false, .ma = 0}},
		.linkloss = {.pwr = {.en = false, .ma = 0}},
		.fault = {.pwr = {.en = false, .ma = 0}},
		.limits = {.current_limit_ma = 1000},
	};
	zassert_equal(entelea_safety_register_channel(&pwr_a), ENTELEA_OK);
	zassert_equal(entelea_safety_register_channel(&pwr_b), ENTELEA_OK);
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);

	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("power:write:0", &t), ENTELEA_OK);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 3, &t), ENTELEA_OK);
	entelea_power_test_set_budget(500);

	/* 无 SET 门 = E_PERM。 */
	entelea_perm_table_t empty;

	entelea_perm_table_init(&empty);
	entelea_principal_t nope;

	zassert_equal(entelea_principal_bind(&nope, 4, &empty), ENTELEA_OK);
	zassert_equal(entelea_power_request(nope, "pwra",
					    (entelea_out_value_t){.pwr = {true, 100}}),
		      ENTELEA_E_PERM);

	/* 预算内两笔；第三笔超预算整笔拒绝（既有槽不受影响）。 */
	zassert_equal(entelea_power_request(ctx, "pwra",
					    (entelea_out_value_t){.pwr = {true, 300}}),
		      ENTELEA_OK);
	zassert_equal(entelea_power_request(ctx, "pwrb",
					    (entelea_out_value_t){.pwr = {true, 150}}),
		      ENTELEA_OK);
	entelea_power_snapshot_t snap;

	entelea_power_snapshot(&snap);
	zassert_equal(snap.used_ma, 450);
	zassert_equal(snap.peak_ma, 450);
	zassert_equal(
		entelea_power_request(ctx, "pwrb",
				      (entelea_out_value_t){.pwr = {true, 400}}),
		ENTELEA_E_RANGE, "450+400 > 500 整笔拒");
	entelea_power_snapshot(&snap);
	zassert_equal(snap.used_ma, 450, "超预算拒绝不改既有");
	/* 本通道替换语义：pwra 300→350（总 150+350=500 ≤ 500 边界通过）。 */
	zassert_equal(entelea_power_request(ctx, "pwra",
					    (entelea_out_value_t){.pwr = {true, 350}}),
		      ENTELEA_OK, "替换自身先扣（边界恰满）");
	entelea_power_snapshot(&snap);
	zassert_equal(snap.used_ma, 500, "边界占用=预算");
	zassert_equal(entelea_power_request(ctx, "pwra",
					    (entelea_out_value_t){.pwr = {true, 351}}),
		      ENTELEA_E_RANGE, "边界+1 拒（替换语义仍受限）");
	/* 关闭不受预算限。 */
	zassert_equal(entelea_power_request(ctx, "pwra",
					    (entelea_out_value_t){.pwr = {false, 0}}),
		      ENTELEA_OK);
	/* 预算 0（未配置）= 一切供电请求拒绝（缺省安全侧）。 */
	entelea_power_test_set_budget(0);
	zassert_equal(entelea_power_request(ctx, "pwrb",
					    (entelea_out_value_t){.pwr = {true, 10}}),
		      ENTELEA_E_RANGE);
	entelea_principal_unbind(&ctx);
	entelea_principal_unbind(&nope);
}

/* ---- 仲裁：席位/压制/幂等 ---- */
ZTEST(framework_r04, test_arbiter_suppression)
{
	static const entelea_out_channel_t ch = {
		.uid = "arbb", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	zassert_equal(entelea_safety_register_channel(&ch), ENTELEA_OK);
	zassert_equal(entelea_safety_set_link(true), ENTELEA_OK);

	zassert_equal(entelea_safety_arb_owner(), ENTELEA_ARB_REMOTE);
	zassert_equal(entelea_safety_commit_src("arbb", (entelea_out_value_t){.b = true},
						ENTELEA_SRC_REMOTE),
		      ENTELEA_OK, "远程席位期远程写放行");
	zassert_equal(entelea_safety_set_arb(ENTELEA_ARB_LOCAL), ENTELEA_OK);
	zassert_equal(entelea_safety_set_arb(ENTELEA_ARB_LOCAL), ENTELEA_OK,
		      "幂等（零事件）");
	zassert_equal(entelea_safety_commit_src("arbb", (entelea_out_value_t){.b = false},
						ENTELEA_SRC_REMOTE),
		      ENTELEA_E_STATE, "本地激活压制远程写");
	zassert_equal(entelea_safety_commit_src("arbb", (entelea_out_value_t){.b = false},
						ENTELEA_SRC_APP),
		      ENTELEA_OK, "APP 编排载体不经闸");
	zassert_equal(entelea_safety_commit_src("arbb", (entelea_out_value_t){.b = true},
						ENTELEA_SRC_LOCAL),
		      ENTELEA_OK, "本地来源放行");
	zassert_equal(entelea_safety_set_arb(ENTELEA_ARB_REMOTE), ENTELEA_OK);
	zassert_equal(entelea_safety_commit_src("arbb", (entelea_out_value_t){.b = true},
						ENTELEA_SRC_REMOTE),
		      ENTELEA_OK, "释放后远程恢复");
	zassert_equal(entelea_safety_set_arb((entelea_arb_owner_t)5),
		      ENTELEA_E_PARAM);
}

/* ---- 截止时间（计划步骤条目，owner A1 决议补交付） ---- */
ZTEST(framework_r04, test_task_deadline)
{
	entelea_task_test_reset();
	entelea_perm_test_reset();
	virt_now = 10000;
	clock_fail_next = false;
	clock_dead = false;
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("gpio:read:0", &t), ENTELEA_OK);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 11, &t), ENTELEA_OK);
	entelea_task_id_t id = 0;

	/* 带 deadline 而未绑时钟 = E_STATE（时间不可知不受理）。 */
	zassert_equal(entelea_task_submit(ctx, "dl0", 1, 10500, &id),
		      ENTELEA_E_STATE);
	zassert_equal(entelea_task_bind_clock(&virt_clock), ENTELEA_OK);
	/* 时钟读失败（单次注入）同面。 */
	clock_fail_next = true;
	zassert_equal(entelea_task_submit(ctx, "dl0", 1, 10500, &id),
		      ENTELEA_E_STATE);
	/* 受理即过期（now=10000 >= deadline=9999）= E_RANGE。 */
	zassert_equal(entelea_task_submit(ctx, "dl0", 1, 9999, &id),
		      ENTELEA_E_RANGE);
	/* deadline=0 无截止路径：不依赖时钟（reset 连绑定一并隔离）。 */
	zassert_equal(entelea_task_bind_clock(&(entelea_clock_t){0}), ENTELEA_E_PARAM,
		      "非法时钟拒绝（read_ms=NULL）");
	entelea_task_test_reset();
	zassert_equal(entelea_task_submit(ctx, "dl0", 1, 0, &id), ENTELEA_OK,
		      "无截止不依赖时钟");
	entelea_task_t view;

	zassert_equal(entelea_task_get(id, &view), ENTELEA_OK);
	zassert_equal(view.deadline_ms, 0, "0=无截止可观测");
	zassert_equal(entelea_task_cancel(ctx, id), ENTELEA_OK);
	zassert_equal(entelea_task_release(ctx, id), ENTELEA_OK);
	zassert_equal(entelea_task_bind_clock(&virt_clock), ENTELEA_OK, "重绑后续用例");

	/* 受理→未到期惰性不过期；到期后线程入口惰性 CANCELLED（效果
	 * NONE）；task_get（[any] 观测）不迁移。 */
	zassert_equal(entelea_task_submit(ctx, "dl1", 2, 10500, &id), ENTELEA_OK);
	virt_now = 10499;
	zassert_equal(entelea_task_start(ctx, id), ENTELEA_OK, "未到期正常开跑");
	virt_now = 10600; /* RUNNING 过期：不自动迁移（效果未知不代决） */
	zassert_equal(entelea_task_get(id, &view), ENTELEA_OK);
	zassert_equal(view.state, ENTELEA_TASK_RUNNING, "RUNNING 过期不代决");
	zassert_equal(view.deadline_ms, 10500, "deadline 可观测");
	zassert_equal(entelea_task_cancel(ctx, id), ENTELEA_OK, "显式取消可用");
	zassert_equal(entelea_task_get(id, &view), ENTELEA_OK);
	zassert_equal(view.state, ENTELEA_TASK_CANCELLED);
	zassert_equal(view.effect, ENTELEA_EFFECT_UNKNOWN, "RUNNING 取消=UNKNOWN");
	zassert_equal(entelea_task_release(ctx, id), ENTELEA_OK);

	/* ACCEPTED 过期：start 惰性转 CANCELLED（NONE）→ 后续动作 E_STATE；
	 * 过期任务不再占受理去重位（同 seq 可重新受理）。 */
	zassert_equal(entelea_task_submit(ctx, "dl2", 3, 11000, &id), ENTELEA_OK);
	virt_now = 12000;
	zassert_equal(entelea_task_start(ctx, id), ENTELEA_E_STATE,
		      "过期 ACCEPTED 惰性取消（start 拒）");
	zassert_equal(entelea_task_get(id, &view), ENTELEA_OK);
	zassert_equal(view.state, ENTELEA_TASK_CANCELLED, "自动取消");
	zassert_equal(view.effect, ENTELEA_EFFECT_NONE, "未开始=无效果");
	zassert_equal(entelea_task_succeed(ctx, id, true), ENTELEA_E_STATE);
	zassert_equal(entelea_task_release(ctx, id), ENTELEA_OK, "过期终态可回收");
	entelea_task_id_t id2 = 0;

	zassert_equal(entelea_task_submit(ctx, "dl2", 3, 0, &id2), ENTELEA_OK,
		      "同 seq 过期终态后可重新受理");
	/* 去重位惰性释放：未过期同 seq 仍去重；过期瞬间即让位。 */
	zassert_equal(entelea_task_submit(ctx, "dl3", 4, 15000, &id), ENTELEA_OK);
	virt_now = 14000;
	zassert_equal(entelea_task_submit(ctx, "dl3", 4, 16000, &id2),
		      ENTELEA_E_BUSY, "未过期同 seq 仍去重");
	virt_now = 15000;
	zassert_equal(entelea_task_submit(ctx, "dl3", 4, 16000, &id2), ENTELEA_OK,
		      "过期即让位（受理扫描内惰性取消）");
	entelea_task_test_reset();
	entelea_perm_test_reset();
}

ZTEST_SUITE(framework_r04, NULL, r04_setup, NULL, NULL, NULL);
