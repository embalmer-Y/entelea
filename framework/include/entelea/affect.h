/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_AFFECT_H
#define ENTELEA_AFFECT_H

/* R11 模拟智能/情感/适应合同见 framework/services/affect/contract.md。
 * 驱力/情感单写者+时间演进；经历驱动适应（版本化+回滚）；安全规则
 * 不可被适应修改；情感关闭不破坏辅助配置。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_AFFECT_LEVELS 4   /* 驱力/情感维度数（建议值） */
#define ENTELEA_ADAPT_HISTORY 8   /* 适应版本环（回滚面） */

/* ---- 驱力/情感（单写者：本模块 tick 独占写） ---- */

typedef struct {
	uint8_t level[ENTELEA_AFFECT_LEVELS]; /* 0..100 */
} entelea_affect_state_t;

/** [thread] 时间演进（单写者——调用方串行；dt 有界：dt≤0 拒）。
 * 规则（建议值，合同登记）：无输入缓回中位、高输入缓升——确定性。 */
entelea_result_t entelea_affect_tick(uint32_t dt_ms,
				     const int8_t stimulus[ENTELEA_AFFECT_LEVELS]);

/** [any] 只读快照。 */
void entelea_affect_get(entelea_affect_state_t *out);

/* ---- 经历驱动适应（版本化+回滚） ---- */

typedef struct {
	uint32_t version;
	uint8_t weight[ENTELEA_AFFECT_LEVELS];
} entelea_adapt_params_t;

/** [thread] 记录一次适应（版本+1；参数由经历驱动——本层只管版本化
 * 与环）；环满=最旧让位（有界）。 */
entelea_result_t entelea_adapt_commit(const uint8_t weight[ENTELEA_AFFECT_LEVELS],
				      entelea_adapt_params_t *out);

/** [thread] 回滚到上一版本（无历史=E_STATE——不静默）。 */
entelea_result_t entelea_adapt_rollback(entelea_adapt_params_t *out);

/** [any] 当前参数。 */
void entelea_adapt_current(entelea_adapt_params_t *out);

#ifdef ENTELEA_TEST
void entelea_affect_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_AFFECT_H */
