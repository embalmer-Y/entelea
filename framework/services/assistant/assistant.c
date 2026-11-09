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
#include <entelea/capability.h>
#include <entelea/events.h>
#include <entelea/netmon.h>
#include <entelea/resource.h>
#include <entelea/task.h>
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
	case ENTELEA_ACT_TASK: {
		/* B-3：任务受理接线（计划 R08 步骤"权限批准->Task 执行->
		 * 来源化反馈"）。受理面=R04 任务机（有界表/受理去重/
		 * deadline 门全复用）；seq=candidate 来源回合序（同
		 * principal+seq 非终态=E_BUSY——重复候选可检测）；
		 * deadline=value_u（0=无截止）。执行驱动（start/succeed/
		 * fail）归上层执行器；**来源化反馈**=task_get 的 effect
		 * 域（R04 合同——本层不复制回执）。task_ret=任务 id。 */
		if (cand->target_uid == NULL) {
			return ENTELEA_E_PARAM; /* target_uid=任务名 */
		}
		entelea_task_id_t id = 0;
		/* seq=source.id 低 16 位（review P2-1 显式化）：任务幂等键
		 * 域=uint16；全序去重政策归上层（回合序回绕 65536 后的
		 * 伪 E_BUSY 方向安全——拒而非静默双受理）。 */
		entelea_result_t r = entelea_task_submit(
			actor, cand->target_uid,
			(uint16_t)(cand->source.id & 0xFFFFU), cand->value_u,
			&id);

		if (r == ENTELEA_OK && task_ret != NULL) {
			*task_ret = (int32_t)id;
		}
		return r;
	}
	case ENTELEA_ACT_OUTPUT: {
		if (cand->target_uid == NULL || cand->value_kind > 2) {
			return ENTELEA_E_PARAM; /* 域外 value_kind 拒（review
						  * P1：不再默认映射为 pwr） */
		}
		/* 权限门（review P1 修复）：按目标描述符的真实类/实例裁决
		 * ——不再以"任一 gpio/pwm write"替代目标面。 */
		const entelea_periph_desc_t *d = NULL;
		entelea_result_t pr = entelea_periph_acquire(actor, cand->target_uid,
							     &d);

		if (pr != ENTELEA_OK) {
			return pr; /* 未注册/越权/非 ACTIVE（模型不能自批） */
		}
		if (cand->value_kind == 2) {
			/* POWER 值必须经 entelea_power_request：power:write
			 * 权限 + 预算检查-提交原子化（review P1：不再旁路）。 */
			entelea_out_value_t pv = {
				.pwr = {.en = cand->value_b, .ma = cand->value_u}};

			return entelea_power_request(actor, cand->target_uid, pv);
		}
		/* 输出动作只打 DEV_OUTPUT 目标（计划符合性审查 P2：原类检
		 * 条件化在 safety_uid!=NULL 内——非输出描述符（acquire 仅
		 * 要求 READ）跳过类检直落 safety_commit，装配撞名时可越权）。*/
		if (d->kind != ENTELEA_DEV_OUTPUT) {
			return ENTELEA_E_PARAM;
		}
		/* 类匹配（无条件）：value_kind 0=GPIO/1=PWM 与目标描述符权限类
		 * 一致（review P1：跨类写拒）。DEV_OUTPUT 注册强制 safety_uid
		 * 非空——描述符即权威类面。 */
		if (cand->value_kind == 0 && d->perm_class != ENTELEA_PERM_GPIO) {
			return ENTELEA_E_PARAM; /* GPIO 值打 PWM 目标 */
		}
		if (cand->value_kind == 1 && d->perm_class != ENTELEA_PERM_PWM) {
			return ENTELEA_E_PARAM; /* PWM 值打 GPIO 目标 */
		}
		entelea_out_value_t v;

		if (cand->value_kind == 0) {
			v = (entelea_out_value_t){.b = cand->value_b};
		} else {
			v = (entelea_out_value_t){.u = cand->value_u};
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
