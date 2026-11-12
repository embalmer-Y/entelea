/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_AFFECT_H
#define ENTELEA_AFFECT_H

/* R11 simulated intelligence / affect / adaptation contract: see
 * framework/services/affect/contract.md.
 * Drive/affect has a single writer and evolves over time. Experience-driven
 * adaptation is versioned, with rollback. Safety rules cannot be changed by
 * adaptation. Turning affect off must not break the auxiliary-intelligence
 * configuration. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_AFFECT_LEVELS 4 /* drive/affect dimension count (suggested) */
#define ENTELEA_ADAPT_HISTORY 8 /* version ring for adaptation (rollback) */

/* ---- Drive/affect (single writer: only this module's tick writes) ---- */

typedef struct {
	uint8_t level[ENTELEA_AFFECT_LEVELS]; /* 0..100 */
} entelea_affect_state_t;

/** [thread] Time evolution. Single writer: callers serialize. dt is bounded;
 * dt <= 0 is rejected. Rule (suggested value, registered in the contract):
 * with no input, values slowly return to the midpoint; with high input, they
 * rise slowly. Deterministic. */
entelea_result_t
entelea_affect_tick(uint32_t dt_ms,
		    const int8_t stimulus[ENTELEA_AFFECT_LEVELS]);

/** [any] Read-only snapshot. */
void
entelea_affect_get(entelea_affect_state_t *out);

/* ---- Experience-driven adaptation (versioned, with rollback) ---- */

typedef struct {
	uint32_t version;
	uint8_t weight[ENTELEA_AFFECT_LEVELS];
} entelea_adapt_params_t;

/** [thread] Records one adaptation (version + 1). Parameters are driven by
 * experience; this layer only handles versioning and the ring. When the ring
 * is full, the oldest entry is evicted (bounded). */
entelea_result_t
entelea_adapt_commit(const uint8_t weight[ENTELEA_AFFECT_LEVELS],
		     entelea_adapt_params_t *out);

/** [thread] Rolls back to the previous version. With no history, returns
 * E_STATE (never silent). */
entelea_result_t
entelea_adapt_rollback(entelea_adapt_params_t *out);

/** [any] Current parameters. */
void
entelea_adapt_current(entelea_adapt_params_t *out);

#ifdef ENTELEA_TEST
void
entelea_affect_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_AFFECT_H */
