/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 任务状态机（R04 新实现，合同 framework/execution/task/contract.md；
 * N-TASK 新-only）。有界表 + 受理去重 + 取消效果域 + 终态回收。
 * accepted≠completed；取消不撤回已发生效果；unknown 非幂等禁止自动重试。
 * 锁：单 mutex（submit/start/终态/回收串行化；查询无锁——观测容忍
 * 微撕裂，权威态经同一查询面）。无模型：测试 principal 直调闭环。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/task.h>

static struct {
	entelea_task_t tasks[ENTELEA_TASK_MAX];
	bool used[ENTELEA_TASK_MAX];
	uint16_t next_seq;
} tb;
static struct k_mutex task_lock = Z_MUTEX_INITIALIZER(task_lock);
static const entelea_clock_t *task_clock;

entelea_result_t entelea_task_bind_clock(const entelea_clock_t *clock)
{
	if (clock == NULL || clock->read_ms == NULL || clock->clock_id == 0) {
		return ENTELEA_E_PARAM;
	}
	task_clock = clock;
	return ENTELEA_OK;
}

/* deadline 判定的"now"：读失败=时间不可知（返回 false——不受理/
 * 不动作，非伪造时间）。deadline 是单点比较（无差值运算）——不
 * 需要三模块式单调护栏簿记；回退至多延迟过期观测（保守方向）。 */
static bool task_now_ms(uint64_t *out)
{
	return task_clock != NULL &&
	       task_clock->read_ms(task_clock->user, out) == ENTELEA_OK;
}

/* 惰性过期（须持 task_lock）：ACCEPTED 且 deadline 已过 → CANCELLED
 * +效果 NONE（未开始无副作用）。RUNNING 不迁移（效果未知不代决）。 */
static void expire_locked(entelea_task_t *t)
{
	if (t->deadline_ms == 0 ||
	    t->state != ENTELEA_TASK_ACCEPTED) {
		return;
	}
	uint64_t now;

	if (!task_now_ms(&now) || now < t->deadline_ms) {
		return; /* 时间不可知/未到期：不动作（保守） */
	}
	t->state = ENTELEA_TASK_CANCELLED;
	t->effect = ENTELEA_EFFECT_NONE;
}

static int find_id(entelea_task_id_t id)
{
	if (id == 0 || id > ENTELEA_TASK_MAX) {
		return -1;
	}
	return tb.used[id - 1] ? (int)(id - 1) : -1;
}

static bool same_principal(entelea_principal_t a, entelea_principal_t b)
{
	return a._handle == b._handle && a.app_id == b.app_id;
}

entelea_result_t entelea_task_submit(entelea_principal_t ctx, const char *name,
				     uint16_t seq, uint64_t deadline_ms,
				     entelea_task_id_t *out)
{
	if (name == NULL) {
		return ENTELEA_E_PARAM;
	}
	/* out 仅在成功时写入（失败不清零——调用方已持有的任务 id 不受
	 * 重复受理拒绝影响）。 */
	/* deadline 受理门（计划步骤"截止时间"）：带截止必须可判时——
	 * 未绑时钟/读失败=E_STATE（fail-closed 不受理）；已过期=E_RANGE。 */
	if (deadline_ms != 0) {
		uint64_t now;

		if (!task_now_ms(&now)) {
			return ENTELEA_E_STATE;
		}
		if (now >= deadline_ms) {
			return ENTELEA_E_RANGE; /* 受理即过期：拒（不养僵尸任务） */
		}
	}
	k_mutex_lock(&task_lock, K_FOREVER);
	/* 受理去重：同 principal 同 seq 的非终态任务 = E_BUSY（重复请求
	 * 可检测，不静默合并——重启重试语义由上层用新 seq）。 */
	for (int i = 0; i < ENTELEA_TASK_MAX; i++) {
		if (tb.used[i]) {
			expire_locked(&tb.tasks[i]); /* 过期任务不再占去重位 */
		}
		if (tb.used[i] && same_principal(tb.tasks[i].owner, ctx) &&
		    tb.tasks[i].seq == seq &&
		    (tb.tasks[i].state == ENTELEA_TASK_ACCEPTED ||
		     tb.tasks[i].state == ENTELEA_TASK_RUNNING)) {
			k_mutex_unlock(&task_lock);
			return ENTELEA_E_BUSY;
		}
	}
	for (int i = 0; i < ENTELEA_TASK_MAX; i++) {
		if (!tb.used[i]) {
			tb.used[i] = true;
			tb.tasks[i].id = (entelea_task_id_t)(i + 1);
			tb.tasks[i].state = ENTELEA_TASK_ACCEPTED;
			tb.tasks[i].effect = ENTELEA_EFFECT_NONE;
			tb.tasks[i].owner = ctx;
			tb.tasks[i].seq = seq;
			tb.tasks[i].deadline_ms = deadline_ms;
			strncpy(tb.tasks[i].name, name, ENTELEA_TASK_NAME_MAX - 1);
			tb.tasks[i].name[ENTELEA_TASK_NAME_MAX - 1] = '\0';
			if (out != NULL) {
				*out = tb.tasks[i].id;
			}
			k_mutex_unlock(&task_lock);
			return ENTELEA_OK;
		}
	}
	k_mutex_unlock(&task_lock);
	return ENTELEA_E_NOMEM; /* 槽满（有界——不排队） */
}

static entelea_result_t transition_effect(entelea_principal_t ctx,
					  entelea_task_id_t id,
					  entelea_task_state_t expect_from,
					  entelea_task_state_t next,
					  bool flag)
{
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	expire_locked(t); /* 惰性过期先于状态断言（过期后动作=E_STATE） */
	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM; /* 他者任务 */
	}
	if (t->state != expect_from) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_STATE; /* 状态竞态：显式拒绝 */
	}
	t->state = next;
	/* 效果域在锁内写入（review P1 修复：解锁后写可能被 release+重用
	 * 竞态覆盖到新任务——不再可能伪造确认）。 */
	if (next == ENTELEA_TASK_SUCCEEDED) {
		t->effect = flag ? ENTELEA_EFFECT_CONFIRMED
				 : ENTELEA_EFFECT_NONE;
	} else {
		t->effect = flag ? ENTELEA_EFFECT_UNKNOWN
				 : ENTELEA_EFFECT_NONE;
	}
	k_mutex_unlock(&task_lock);
	return ENTELEA_OK;
}

static entelea_result_t transition(entelea_principal_t ctx, entelea_task_id_t id,
				   entelea_task_state_t expect_from,
				   entelea_task_state_t next)
{
	/* 状态迁移（无效果域面——start 等中间迁移用）。 */
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	expire_locked(t); /* 惰性过期先于状态断言 */
	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM;
	}
	if (t->state != expect_from) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_STATE;
	}
	t->state = next;
	k_mutex_unlock(&task_lock);
	return ENTELEA_OK;
}

entelea_result_t entelea_task_start(entelea_principal_t ctx, entelea_task_id_t id)
{
	return transition(ctx, id, ENTELEA_TASK_ACCEPTED, ENTELEA_TASK_RUNNING);
}

static entelea_result_t finish(entelea_principal_t ctx, entelea_task_id_t id,
			       entelea_task_state_t st, bool flag)
{
	/* 效果域由调用方据真实回执给出——本层不伪造确认；写入在锁内。 */
	return transition_effect(ctx, id, ENTELEA_TASK_RUNNING, st, flag);
}

entelea_result_t entelea_task_succeed(entelea_principal_t ctx,
				      entelea_task_id_t id, bool effect_confirmed)
{
	return finish(ctx, id, ENTELEA_TASK_SUCCEEDED, effect_confirmed);
}

entelea_result_t entelea_task_fail(entelea_principal_t ctx, entelea_task_id_t id,
				   bool effect_unknown)
{
	return finish(ctx, id, ENTELEA_TASK_FAILED, effect_unknown);
}

entelea_result_t entelea_task_cancel(entelea_principal_t ctx, entelea_task_id_t id)
{
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	expire_locked(t); /* 已过期=已 CANCELLED → 下方终态分支拒 */
	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM;
	}
	if (t->state == ENTELEA_TASK_ACCEPTED) {
		t->state = ENTELEA_TASK_CANCELLED;
		t->effect = ENTELEA_EFFECT_NONE; /* 未开始：无效果 */
		k_mutex_unlock(&task_lock);
		return ENTELEA_OK;
	}
	if (t->state == ENTELEA_TASK_RUNNING) {
		t->state = ENTELEA_TASK_CANCELLED;
		t->effect = ENTELEA_EFFECT_UNKNOWN; /* 已发生效果不撤回 */
		k_mutex_unlock(&task_lock);
		return ENTELEA_OK;
	}
	k_mutex_unlock(&task_lock);
	return ENTELEA_E_STATE; /* 终态后取消 */
}

entelea_result_t entelea_task_get(entelea_task_id_t id, entelea_task_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	int i = find_id(id);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	*out = tb.tasks[i]; /* 无锁快照（观测容忍微撕裂） */
	return ENTELEA_OK;
}

entelea_result_t entelea_task_release(entelea_principal_t ctx, entelea_task_id_t id)
{
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM;
	}
	expire_locked(t); /* 过期 ACCEPTED 先转终态（可回收） */
	if (t->state == ENTELEA_TASK_ACCEPTED || t->state == ENTELEA_TASK_RUNNING) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_STATE; /* 未终态不可回收 */
	}
	tb.used[i] = false;
	memset(t, 0, sizeof(*t));
	k_mutex_unlock(&task_lock);
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_task_test_reset(void)
{
	memset(&tb, 0, sizeof(tb));
	task_clock = NULL; /* 连时钟绑定一并隔离（用例自含重绑） */
}
#endif
