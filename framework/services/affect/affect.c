/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R11 模拟智能/情感/适应（新-only N-ADAPTATION；合同见本目录
 * contract.md）。驱力/情感单写者（tick 独占）+确定性时间演进；
 * 适应=版本环（回滚面）——**安全规则不可被适应修改**（本层无任何
 * 权限/安全面写路径——结构性保证）；情感关闭（零 stimulus 停摆）
 * 不破坏辅助配置（R08 面独立）。产品参数与共享机制分开（本层
 * 只管机制；参数语义归产品）。
 */
#include <string.h>
#include <entelea/affect.h>

static entelea_affect_state_t affect;
static entelea_adapt_params_t adapt_history[ENTELEA_ADAPT_HISTORY];
static uint32_t adapt_len;
static uint32_t adapt_version;

/* 确定性演进（建议值规则）：stimulus>0 升、<0 降、0 回中位（50）。
 * 步长 = |stimulus| 与 dt/1000 的饱和积（上限 10/步——变化有界）。 */
#define AFFECT_MIDPOINT 50
#define AFFECT_STEP_MAX 10

entelea_result_t entelea_affect_tick(uint32_t dt_ms,
				     const int8_t stimulus[ENTELEA_AFFECT_LEVELS])
{
	if (stimulus == NULL || dt_ms == 0) {
		return ENTELEA_E_PARAM;
	}
	uint32_t scale = dt_ms / 1000U;

	if (scale == 0) {
		scale = 1; /* 亚秒 tick 至少 1 步（时间推进不丢） */
	}
	if (scale > AFFECT_STEP_MAX) {
		scale = AFFECT_STEP_MAX; /* 步长饱和（变化有界） */
	}
	for (int i = 0; i < ENTELEA_AFFECT_LEVELS; i++) {
		int v = affect.level[i];
		int s = stimulus[i];

		if (s > 0) {
			v += (int)scale;
			if (v > 100) {
				v = 100;
			}
		} else if (s < 0) {
			v -= (int)scale;
			if (v < 0) {
				v = 0;
			}
		} else {
			/* 回中位（确定性衰减）。 */
			if (v > AFFECT_MIDPOINT) {
				v -= (int)scale;
				if (v < AFFECT_MIDPOINT) {
					v = AFFECT_MIDPOINT;
				}
			} else if (v < AFFECT_MIDPOINT) {
				v += (int)scale;
				if (v > AFFECT_MIDPOINT) {
					v = AFFECT_MIDPOINT;
				}
			}
		}
		affect.level[i] = (uint8_t)v;
	}
	return ENTELEA_OK;
}

void entelea_affect_get(entelea_affect_state_t *out)
{
	if (out != NULL) {
		*out = affect;
	}
}

/* ---- 适应版本环 ---- */

entelea_result_t entelea_adapt_commit(
	const uint8_t weight[ENTELEA_AFFECT_LEVELS],
	entelea_adapt_params_t *out)
{
	if (weight == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (adapt_len == ENTELEA_ADAPT_HISTORY) {
		memmove(&adapt_history[0], &adapt_history[1],
			(adapt_len - 1) * sizeof(adapt_history[0]));
		adapt_len--;
	}
	adapt_version++;
	adapt_history[adapt_len].version = adapt_version;
	memcpy(adapt_history[adapt_len].weight, weight,
	       ENTELEA_AFFECT_LEVELS);
	adapt_len++;
	if (out != NULL) {
		*out = adapt_history[adapt_len - 1];
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_adapt_rollback(entelea_adapt_params_t *out)
{
	if (adapt_len <= 1) {
		return ENTELEA_E_STATE; /* 无历史可回（不静默成功） */
	}
	adapt_len--; /* 当前版本出环（回滚不删除经历——R10 面） */
	if (out != NULL) {
		*out = adapt_history[adapt_len - 1];
	}
	return ENTELEA_OK;
}

void entelea_adapt_current(entelea_adapt_params_t *out)
{
	if (out != NULL && adapt_len > 0) {
		*out = adapt_history[adapt_len - 1];
	} else if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
}

#ifdef ENTELEA_TEST
void entelea_affect_test_reset(void)
{
	memset(&affect, 0, sizeof(affect));
	/* 初始情感=中位（确定性基线）。 */
	for (int i = 0; i < ENTELEA_AFFECT_LEVELS; i++) {
		affect.level[i] = AFFECT_MIDPOINT;
	}
	memset(adapt_history, 0, sizeof(adapt_history));
	adapt_len = 0;
	adapt_version = 0;
}
#endif
