/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R11 simulated intelligence: affect, emotion and adaptation (new-only
 * N-ADAPTATION; see contract.md in this directory). Drive and emotion
 * have a single writer (tick-exclusive) with deterministic time
 * evolution. Adaptation is a version ring (the rollback surface);
 * **safety rules cannot be changed by adaptation**. This layer has no
 * permission or safety write path (a structural guarantee). Turning
 * emotion off (zero stimulus freezes it) does not break auxiliary
 * configuration (the R08 surface is independent). Product parameters
 * are kept apart from shared mechanisms: this layer handles mechanism
 * only; parameter semantics belong to the product.
 */
#include <string.h>
#include <entelea/affect.h>

#define AFFECT_MIDPOINT 50
#define AFFECT_STEP_MAX 10

_Static_assert(ENTELEA_AFFECT_LEVELS == 4, "update initial affect levels");
static entelea_affect_state_t affect = {
	.level = { AFFECT_MIDPOINT, AFFECT_MIDPOINT, AFFECT_MIDPOINT,
		   AFFECT_MIDPOINT },
};
static entelea_adapt_params_t adapt_history[ENTELEA_ADAPT_HISTORY];
static uint32_t adapt_len;
static uint32_t adapt_version;

/* Deterministic evolution (advisory rule): stimulus>0 raises, <0 lowers,
 * 0 returns to the midpoint (50). Step is the saturating product of
 * |stimulus| and dt/1000 (at most 10 per step, so change is bounded). */
entelea_result_t
entelea_affect_tick(uint32_t dt_ms,
		    const int8_t stimulus[ENTELEA_AFFECT_LEVELS])
{
	if (stimulus == NULL || dt_ms == 0) {
		return ENTELEA_E_PARAM;
	}
	uint32_t scale = dt_ms / 1000U;

	if (scale == 0) {
		scale = 1; /* Sub-second tick: at least 1 step, time not lost */
	}
	if (scale > AFFECT_STEP_MAX) {
		scale = AFFECT_STEP_MAX; /* Saturate step (bounded change) */
	}
	for (int i = 0; i < ENTELEA_AFFECT_LEVELS; i++) {
		int v = affect.level[i];
		int s = stimulus[i];

		if (s > 0) {
			v += (int) scale;
			if (v > 100) {
				v = 100;
			}
		} else if (s < 0) {
			v -= (int) scale;
			if (v < 0) {
				v = 0;
			}
		} else {
			/* Decay back to the midpoint (deterministic) */
			if (v > AFFECT_MIDPOINT) {
				v -= (int) scale;
				if (v < AFFECT_MIDPOINT) {
					v = AFFECT_MIDPOINT;
				}
			} else if (v < AFFECT_MIDPOINT) {
				v += (int) scale;
				if (v > AFFECT_MIDPOINT) {
					v = AFFECT_MIDPOINT;
				}
			}
		}
		affect.level[i] = (uint8_t) v;
	}
	return ENTELEA_OK;
}

void
entelea_affect_get(entelea_affect_state_t *out)
{
	if (out != NULL) {
		*out = affect;
	}
}

/* ---- Adaptation version ring ---- */

entelea_result_t
entelea_adapt_commit(const uint8_t weight[ENTELEA_AFFECT_LEVELS],
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
	memcpy(adapt_history[adapt_len].weight, weight, ENTELEA_AFFECT_LEVELS);
	adapt_len++;
	if (out != NULL) {
		*out = adapt_history[adapt_len - 1];
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_adapt_rollback(entelea_adapt_params_t *out)
{
	if (adapt_len <= 1) {
		return ENTELEA_E_STATE; /* No history; no silent success */
	}
	adapt_len--; /* Drop current version; rollback keeps history (R10) */
	if (out != NULL) {
		*out = adapt_history[adapt_len - 1];
	}
	return ENTELEA_OK;
}

void
entelea_adapt_current(entelea_adapt_params_t *out)
{
	if (out != NULL && adapt_len > 0) {
		*out = adapt_history[adapt_len - 1];
	} else if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
}

#ifdef ENTELEA_TEST
void
entelea_affect_test_reset(void)
{
	memset(&affect, 0, sizeof(affect));
	/* Initial affect = midpoint (deterministic baseline) */
	for (int i = 0; i < ENTELEA_AFFECT_LEVELS; i++) {
		affect.level[i] = AFFECT_MIDPOINT;
	}
	memset(adapt_history, 0, sizeof(adapt_history));
	adapt_len = 0;
	adapt_version = 0;
}
#endif
