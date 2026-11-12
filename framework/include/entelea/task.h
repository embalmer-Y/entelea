/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_TASK_H
#define ENTELEA_TASK_H

/* R04 task state machine contract: see
 * framework/execution/capability/contract.md.
 * accepted is not completed; cancellation does not undo effects that already
 * happened; an unknown outcome of a non-idempotent action must not be retried
 * automatically. A full closed loop can run without a model, through restricted
 * test calls. The task table is bounded. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/app.h> /* receipt effect semantics */
#include <entelea/principal.h>
#include <entelea/result.h>
#include <entelea/time.h> /* deadline time base (instance clock injection) */

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_TASK_MAX 4 /* bounded (DEC-27 #15 semantics) */
#define ENTELEA_TASK_NAME_MAX 16

typedef enum {
	ENTELEA_TASK_ACCEPTED = 0, /* accepted (not yet started) */
	ENTELEA_TASK_RUNNING,
	ENTELEA_TASK_SUCCEEDED,
	ENTELEA_TASK_FAILED,
	ENTELEA_TASK_CANCELLED,
	ENTELEA_TASK_STATE_COUNT,
} entelea_task_state_t;

/* The effect domain reuses entelea_receipt_effect_t from R01 app.h, the single
 * semantic source. */

typedef uint8_t entelea_task_id_t; /* 0 = invalid, 1..MAX */

typedef struct {
	entelea_task_id_t id;
	entelea_task_state_t state;
	entelea_effect_t effect;
	entelea_principal_t owner; /* principal at acceptance (copy) */
	uint16_t seq; /* acceptance sequence (detects duplicates) */
	uint64_t deadline_ms; /* absolute ms (instance clock); 0 = none */
	char name[ENTELEA_TASK_NAME_MAX];
} entelea_task_t;

/** [boot] Binds the instance clock, the time base for deadline checks and the
 * acceptance gate. The deadline=0 path does not depend on it. Deadline
 * semantics (plan step "deadline" item; owner decision A1, delivered as a
 * supplement): a deadline already expired at acceptance returns E_RANGE. If
 * the clock is unbound, a read fails, or time goes backwards for a task with a
 * deadline, returns E_STATE (unknown time means no acceptance and no start).
 * While a non-terminal task with a deadline exists, binding a different
 * clock_id is refused (E_BUSY). Expiry during ACCEPTED lazily becomes
 * CANCELLED with effect NONE (nothing started, so no side effects). There is
 * no timer; the check runs at thread entry, per the same determinism
 * discipline as R06. Expiry during RUNNING does **not** transition
 * automatically: the outcome is unknown and must not be decided on the
 * caller's behalf. The caller observes the deadline via task_get and then
 * cancels explicitly. */
entelea_result_t
entelea_task_bind_clock(const entelea_clock_t *clock);

/** [thread] Accepts a task (idempotency: the same principal with the same seq
 * and a non-terminal state returns E_BUSY; duplicate acceptance is refused,
 * not silently merged). A full table returns E_NOMEM. deadline_ms is an
 * absolute deadline in ms (0 = none). */
entelea_result_t
entelea_task_submit(entelea_principal_t ctx, const char *name, uint16_t seq,
		    uint64_t deadline_ms, entelea_task_id_t *out);

/** [thread] Starts execution (ACCEPTED -> RUNNING). A task owned by another
 * principal, in a terminal state, or missing returns E_STATE. */
entelea_result_t
entelea_task_start(entelea_principal_t ctx, entelea_task_id_t id);

/** [thread] Terminal state: success. The caller supplies effect = CONFIRMED
 * from the real receipt; this layer does not fabricate confirmation. */
entelea_result_t
entelea_task_succeed(entelea_principal_t ctx, entelea_task_id_t id,
		     bool effect_confirmed);

/** [thread] Failure. effect_unknown=true if and only if the failure point is
 * after the side effect (the caller decides; an unknown non-idempotent action
 * must not be retried automatically; this layer sets the flag and reports it
 * as is). */
entelea_result_t
entelea_task_fail(entelea_principal_t ctx, entelea_task_id_t id,
		  bool effect_unknown);

/** [thread] Cancels a task. Cancelling during ACCEPTED gives effect NONE;
 * cancelling during RUNNING gives effect UNKNOWN (effects that already happened
 * are not undone; the caller must not use cancellation in place of rollback).
 * Cancelling after a terminal state returns E_STATE. */
entelea_result_t
entelea_task_cancel(entelea_principal_t ctx, entelea_task_id_t id);

/** [any] Queries a task (including the effect domain). */
entelea_result_t
entelea_task_get(entelea_task_id_t id, entelea_task_t *out);

/** [thread] Releases a terminal task so its slot can be reused. RUNNING and
 * ACCEPTED tasks are refused. */
entelea_result_t
entelea_task_release(entelea_principal_t ctx, entelea_task_id_t id);

#ifdef ENTELEA_TEST
void
entelea_task_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_TASK_H */
