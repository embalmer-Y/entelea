/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_TASK_H
#define ENTELEA_TASK_H

/* R04 任务状态机合同见 framework/execution/task/contract.md。
 * accepted≠completed；取消不撤回已发生效果；unknown 非幂等动作禁止
 * 自动重试；无模型也可由受限测试调用完整闭环。有界任务表。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/app.h>  /* receipt effect 语义 */
#include <entelea/principal.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_TASK_MAX 4 /* 有界（DEC-27 #15 语义面） */
#define ENTELEA_TASK_NAME_MAX 16

typedef enum {
	ENTELEA_TASK_ACCEPTED = 0, /* 已受理（未开始执行） */
	ENTELEA_TASK_RUNNING,
	ENTELEA_TASK_SUCCEEDED,
	ENTELEA_TASK_FAILED,
	ENTELEA_TASK_CANCELLED,
	ENTELEA_TASK_STATE_COUNT,
} entelea_task_state_t;

/* 效果域复用 R01 app.h 的 entelea_receipt_effect_t（单一语义源）。 */

typedef uint8_t entelea_task_id_t; /* 0=无效，1..MAX */

typedef struct {
	entelea_task_id_t id;
	entelea_task_state_t state;
	entelea_effect_t effect;
	entelea_principal_t owner;    /* 受理时的 principal（副本） */
	uint16_t seq;                /* 受理序（重复请求可检测） */
	char name[ENTELEA_TASK_NAME_MAX];
} entelea_task_t;

/** [thread] 受理（idempotency：同一 principal+同 seq 未终态 = E_BUSY——
 * 重复受理拒绝，不静默合并）。槽满 = E_NOMEM。 */
entelea_result_t entelea_task_submit(entelea_principal_t ctx, const char *name,
				     uint16_t seq, entelea_task_id_t *out);

/** [thread] 开始执行（ACCEPTED→RUNNING；他者/终态/不存在 = E_STATE）。 */
entelea_result_t entelea_task_start(entelea_principal_t ctx, entelea_task_id_t id);

/** [thread] 终态：成功（effect=CONFIRMED 由调用方据真实回执给出——
 * 本层不伪造确认）。 */
entelea_result_t entelea_task_succeed(entelea_principal_t ctx,
				      entelea_task_id_t id,
				      bool effect_confirmed);

/** [thread] 失败。effect_unknown=true 当且仅当失败点在副作用之后
 * （调用方判定；unknown 非幂等动作禁止自动重试——本层置位并如实呈现）。 */
entelea_result_t entelea_task_fail(entelea_principal_t ctx,
				   entelea_task_id_t id,
				   bool effect_unknown);

/** [thread] 取消：ACCEPTED 期取消 = 效果 NONE；RUNNING 期取消 =
 * 效果 UNKNOWN（已发生效果不撤回——调用方不得以取消替代回滚）。
 * 终态后取消 = E_STATE。 */
entelea_result_t entelea_task_cancel(entelea_principal_t ctx,
				     entelea_task_id_t id);

/** [any] 查询（含 effect 域）。 */
entelea_result_t entelea_task_get(entelea_task_id_t id, entelea_task_t *out);

/** [thread] 终态任务回收（槽复用；RUNNING/ACCEPTED 拒绝）。 */
entelea_result_t entelea_task_release(entelea_principal_t ctx,
				      entelea_task_id_t id);

#ifdef ENTELEA_TEST
void entelea_task_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_TASK_H */
