/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R08 最小辅助闭环（新-only N-CONTEXT/N-DELIBERATION；合同见本目录
 * contract.md）。事件→有界上下文→触发→候选→权限→执行→反馈。
 * 模型=建议者：本层组装的候选**必须**经 R04 principal 裁决 + R02
 * 保护层——模型输入整数不能构造 principal（R01 合同强制点）。
 * 云依赖：模型/网络任一 DOWN → assistant_available=false（停智能；
 * 独立 App 照常——各自查各自依赖，不查此面）。
 */
#include <string.h>
#include <entelea/assistant.h>
#include <entelea/events.h>
#include <entelea/netmon.h>
#include <entelea/safety.h>

/* ---- 上下文汇聚（有界窗） ---- */

void entelea_ctx_push(entelea_context_window_t *w, const entelea_ctx_event_t *e)
{
	if (w == NULL || e == NULL) {
		return;
	}
	if (w->count < ENTELEA_CTX_EVENTS_MAX) {
		w->events[w->count++] = *e;
	} else {
		/* 窗满弃最旧（环形语义——保持最近上下文）。 */
		memmove(&w->events[0], &w->events[1],
			(w->count - 1) * sizeof(w->events[0]));
		w->events[w->count - 1] = *e;
		w->dropped++;
	}
}

void entelea_ctx_snapshot(const entelea_context_window_t *w,
			  entelea_ctx_event_t *out, size_t max, size_t *n)
{
	if (w == NULL || n == NULL) {
		return;
	}
	size_t take = w->count < max ? w->count : max;

	if (out != NULL) {
		memcpy(out, w->events, take * sizeof(out[0]));
	}
	*n = take;
}

uint32_t entelea_ctx_dropped(const entelea_context_window_t *w)
{
	return w == NULL ? 0 : w->dropped;
}

/* ---- 触发（建议值：窗满 或 显式；无内部定时器——确定性） ---- */

bool entelea_should_deliberate(const entelea_context_window_t *w,
			       uint64_t now_ms, uint64_t *next_due_ms)
{
	(void)now_ms;
	if (next_due_ms != NULL) {
		*next_due_ms = 0; /* 无周期触发（显式/窗满触发——合同） */
	}
	return w != NULL && w->count >= ENTELEA_CTX_EVENTS_MAX;
}

/* ---- 执行门 ---- */

entelea_result_t entelea_assistant_execute(entelea_principal_t actor,
					   const entelea_candidate_t *cand,
					   int32_t *task_ret)
{
	if (task_ret != NULL) {
		*task_ret = 0;
	}
	if (cand == NULL) {
		return ENTELEA_E_PARAM;
	}
	switch (cand->kind) {
	case ENTELEA_ACT_NONE:
		return ENTELEA_OK; /* 无动作（通知类候选由上层消费） */
	case ENTELEA_ACT_NOTIFY:
		return ENTELEA_OK; /* 无副作用——不留执行回执面 */
	case ENTELEA_ACT_OUTPUT: {
		if (cand->target_uid == NULL) {
			return ENTELEA_E_PARAM;
		}
		/* 权限门：ACT_OUTPUT 要求目标描述符的 write 声明（R04 面
		 * ——R05 periph_acquire 在 WASM 侧；宿主侧直接 commit 前
		 * 先查 principal 有任一 gpio/pwm write——简化面，完整
		 * periph 绑定归 R09+ 装配）。 */
		entelea_result_t pr = entelea_perm_check(
			actor, ENTELEA_PERM_GPIO, ENTELEA_PERM_WRITE, 0);

		if (pr != ENTELEA_OK && cand->value_kind != 0) {
			pr = entelea_perm_check(actor, ENTELEA_PERM_PWM,
						ENTELEA_PERM_WRITE, 0);
		}
		if (pr != ENTELEA_OK) {
			return ENTELEA_E_PERM; /* 模型不能自批 */
		}
		entelea_out_value_t v;

		switch (cand->value_kind) {
		case 0:
			v = (entelea_out_value_t){.b = cand->value_b};
			break;
		case 1:
			v = (entelea_out_value_t){.u = cand->value_u};
			break;
		default:
			v = (entelea_out_value_t){.pwr = {.en = cand->value_b,
						       .ma = cand->value_u}};
			break;
		}
		/* 唯一写路径（保护层——无旁路）。 */
		return entelea_safety_commit(cand->target_uid, v);
	}
	default:
		return ENTELEA_E_PARAM;
	}
}

/* ---- 云依赖判定 ---- */

bool entelea_assistant_available(void)
{
	/* D08：模型可用 = 模型依赖 UP 且网络 UP（云模型经网络到达）。 */
	return entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_UP &&
	       entelea_dep_state(ENTELEA_DEP_NET) == ENTELEA_DEP_UP;
}

#ifdef ENTELEA_TEST
void entelea_assistant_test_reset(void)
{
	/* 无模块级可变态（窗由调用方持有）——占位保持接口对称。 */
}
#endif
