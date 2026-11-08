/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_ASSISTANT_H
#define ENTELEA_ASSISTANT_H

/* R08 最小辅助闭环合同见 framework/services/assistant/contract.md。
 * 事件 → 有界上下文汇聚 → 触发 → 结构化候选 → 权限批准 → 任务执行 →
 * 来源化反馈。模型=建议者：不能伪造 principal/来源/能力；执行必经
 * R04 权限门与 R02 保护层。云不可用 → 停用智能（独立 App 照常）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/context.h>
#include <entelea/result.h>
#include <entelea/safety.h>
#include <entelea/principal.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_CTX_EVENTS_MAX 16   /* 有界上下文窗（建议值） */
#define ENTELEA_CANDIDATES_MAX 4    /* 每轮候选上限（有界） */
#define ENTELEA_SUGGESTION_TEXT_MAX 64

/* ---- 上下文汇聚（有界窗；溢出弃最旧+计数） ---- */

typedef struct {
	entelea_event_id_t id;
	uint64_t t_ms;
	const void *data;
	size_t len;
} entelea_ctx_event_t;

typedef struct {
	entelea_ctx_event_t events[ENTELEA_CTX_EVENTS_MAX];
	uint32_t count;    /* 当前窗内条数 */
	uint32_t dropped;  /* 溢出丢弃计数 */
} entelea_context_window_t;

/** [thread] 事件入窗（窗满弃最旧+dropped++——有界）。 */
void entelea_ctx_push(entelea_context_window_t *w, const entelea_ctx_event_t *e);

/** 窗快照（观测面）。 */
void entelea_ctx_snapshot(const entelea_context_window_t *w,
			  entelea_ctx_event_t *out, size_t max, size_t *n);

/** 溢出计数。 */
uint32_t entelea_ctx_dropped(const entelea_context_window_t *w);

/* ---- 结构化候选（模型输出 = 建议载荷，非执行许可） ---- */

typedef enum {
	ENTELEA_ACT_NONE = 0,
	ENTELEA_ACT_OUTPUT,   /* 经保护层的输出（uid+value） */
	ENTELEA_ACT_TASK,     /* 任务受理（R04） */
	ENTELEA_ACT_NOTIFY,   /* 仅通知（无副作用） */
} entelea_action_kind_t;

typedef struct {
	entelea_action_kind_t kind;
	char text[ENTELEA_SUGGESTION_TEXT_MAX]; /* 人读建议说明 */
	entelea_source_t source;                 /* 建议来源（模型=MODEL） */
	const char *target_uid;                 /* ACT_OUTPUT 目标 */
	uint8_t value_kind;                      /* 0=bool 1=u32 2=pwr */
	bool value_b;
	uint32_t value_u;
} entelea_candidate_t;

/* ---- 触发（有界；建议值规则在合同中登记） ---- */

/** [thread] 判定是否触发一轮思考（窗满/超时/显式请求——建议值）。 */
bool entelea_should_deliberate(const entelea_context_window_t *w,
			       uint64_t now_ms, uint64_t *next_due_ms);

/* ---- 执行门（候选 → R04 权限 → R02 保护层） ---- */

/** [thread] 执行候选：无授权/越权 → E_PERM（模型不能自批）；输出经
 *  entelea_safety_commit（保护层唯一写路径）。返回回执效果。 */
entelea_result_t entelea_assistant_execute(entelea_principal_t actor,
					   const entelea_candidate_t *cand,
					   int32_t *task_ret);

/* ---- 云依赖判定（D08：模型不可用 → 停智能；独立 App 照常） ---- */

/** 智脑可用性（= ENTELEA_DEP_MODEL 为 UP 且网络 UP——独立 App 不查此面）。 */
bool entelea_assistant_available(void);

#ifdef ENTELEA_TEST
void entelea_assistant_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_ASSISTANT_H */
