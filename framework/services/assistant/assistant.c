/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R08 minimal assistant loop (new-only N-CONTEXT/N-DELIBERATION; see
 * contract.md in this directory). Event -> bounded context -> trigger ->
 * candidate -> permission -> execution -> feedback.
 * Model = advisor: candidates built here **must** be adjudicated by the
 * R04 principal plus the R02 protection layer. Integer model input cannot
 * construct a principal (the R01 contract enforcement point).
 * Cloud dependency: if the model or network is DOWN, assistant_available=false
 * (intelligence stops; standalone Apps keep running, each checking its own
 * dependencies, not this surface).
 */
#include <string.h>
#include <entelea/assistant.h>
#include <entelea/capability.h>
#include <entelea/events.h>
#include <entelea/netmon.h>
#include <entelea/resource.h>
#include <entelea/task.h>
#include <entelea/safety.h>

/* ---- Context aggregation (bounded window) ---- */

void
entelea_ctx_push(entelea_context_window_t *w, const entelea_ctx_event_t *e)
{
	if (w == NULL || e == NULL) {
		return;
	}
	if (w->count < ENTELEA_CTX_EVENTS_MAX) {
		w->events[w->count++] = *e;
	} else {
		/* Window full: drop oldest (ring keeps recent context) */
		memmove(&w->events[0], &w->events[1],
			(w->count - 1) * sizeof(w->events[0]));
		w->events[w->count - 1] = *e;
		w->dropped++;
	}
}

void
entelea_ctx_snapshot(const entelea_context_window_t *w,
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

uint32_t
entelea_ctx_dropped(const entelea_context_window_t *w)
{
	return w == NULL ? 0 : w->dropped;
}

/* ---- Trigger (advisory): window full or explicit; no timers ---- */

bool
entelea_should_deliberate(const entelea_context_window_t *w, uint64_t now_ms,
			  uint64_t *next_due_ms)
{
	(void) now_ms;
	if (next_due_ms != NULL) {
		*next_due_ms = 0; /* No periodic trigger (explicit or full) */
	}
	return w != NULL && w->count >= ENTELEA_CTX_EVENTS_MAX;
}

/* ---- Execution gate ---- */

entelea_result_t
entelea_assistant_execute(entelea_principal_t actor,
			  const entelea_candidate_t *cand, int32_t *task_ret)
{
	if (task_ret != NULL) {
		*task_ret = 0;
	}
	if (cand == NULL) {
		return ENTELEA_E_PARAM;
	}
	switch (cand->kind) {
	case ENTELEA_ACT_NONE:
		return ENTELEA_OK; /* No-op; notify candidates handled above */
	case ENTELEA_ACT_NOTIFY:
		return ENTELEA_OK; /* No side effects; no receipt surface */
	case ENTELEA_ACT_TASK: {
		/* B-3: task admission wiring (plan R08 steps: "permission
		 * approval -> Task execution -> source-tagged feedback").
		 * Admission surface is the R04 task machine: bounded table,
		 * admission dedup and deadline gate, all reused. seq is the
		 * candidate's source turn order (same principal+seq while
		 * non-terminal is E_BUSY, so duplicates are detectable).
		 * deadline = value_u (0 = no deadline). Execution driving
		 * (start/succeed/fail) is left to the upper executor;
		 * **source-tagged feedback** is the effect domain of task_get
		 * (R04 contract; this layer does not copy receipts). task_ret
		 * is the task id. */
		if (cand->target_uid == NULL) {
			return ENTELEA_E_PARAM; /* target_uid = task name */
		}
		entelea_task_id_t id = 0;
		/* seq = low 16 bits of source.id (made explicit per
		 * review P2-1). The task idempotency key domain is uint16;
		 * full-order dedup policy belongs to the upper layer. After
		 * the turn sequence wraps past 65536, a spurious E_BUSY is
		 * safe: it rejects, never silently double-accepts. */
		entelea_result_t r = entelea_task_submit(
			actor, cand->target_uid,
			(uint16_t) (cand->source.id & 0xFFFFU), cand->value_u,
			&id);

		if (r == ENTELEA_OK && task_ret != NULL) {
			*task_ret = (int32_t) id;
		}
		return r;
	}
	case ENTELEA_ACT_OUTPUT: {
		if (cand->target_uid == NULL || cand->value_kind > 2) {
			return ENTELEA_E_PARAM; /* bad value_kind (review P1:
						 * no pwr default) */
		}
		/* Permission gate (review P1 fix): decide by the target
		 * descriptor's real class/instance, not by "any gpio/pwm
		 * write" standing in for the target surface. */
		const entelea_periph_desc_t *d = NULL;
		entelea_result_t pr =
			entelea_periph_acquire(actor, cand->target_uid, &d);

		if (pr != ENTELEA_OK) {
			return pr; /* unregistered, denied or not ACTIVE
				    * (model cannot self-approve) */
		}
		/* Output actions target only DEV_OUTPUT (plan conformance
		 * review P2): the class check was conditional on
		 * safety_uid!=NULL, so a non-output descriptor (acquire only
		 * needs READ) skipped it and went straight to safety_commit.
		 * A name clash at assembly could then escalate privilege. */
		if (d->kind != ENTELEA_DEV_OUTPUT) {
			return ENTELEA_E_PARAM;
		}
		/* Unconditional class check: value_kind 0=GPIO/1=PWM/2=POWER
		 * must match the target descriptor's permission class (review
		 * P1: cross-class writes refused). DEV_OUTPUT registration
		 * forces a non-empty safety_uid; the descriptor is the
		 * authoritative class surface. */
		static const entelea_perm_class_t want[] = {
			ENTELEA_PERM_GPIO, ENTELEA_PERM_PWM, ENTELEA_PERM_POWER
		};

		if (d->perm_class != want[cand->value_kind]) {
			return ENTELEA_E_PARAM;
		}
		/* Write surface = safety_uid of the adjudicated descriptor
		 * (RF1 P1-2): a logical name may collide with another party's
		 * protection channel; committing by logical name would bypass
		 * this permission decision. */
		if (cand->value_kind == 2) {
			/* POWER values must go through entelea_power_request:
			 * power:write permission, plus an atomic budget
			 * check-and-commit (review P1: no longer bypassed). */
			entelea_out_value_t pv = {
				.pwr = { .en = cand->value_b,
					 .ma = cand->value_u }
			};

			return entelea_power_request(actor, d->safety_uid, pv);
		}
		entelea_out_value_t v;

		if (cand->value_kind == 0) {
			v = (entelea_out_value_t) { .b = cand->value_b };
		} else {
			v = (entelea_out_value_t) { .u = cand->value_u };
		}
		/* Only write path (protection layer; no bypass) */
		return entelea_safety_commit(d->safety_uid, v);
	}
	default:
		return ENTELEA_E_PARAM;
	}
}

/* ---- Cloud dependency check ---- */

bool
entelea_assistant_available(void)
{
	/* D08: model usable = model dependency UP and network UP
	 * (a cloud model is reached through the network). */
	return entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_UP &&
	       entelea_dep_state(ENTELEA_DEP_NET) == ENTELEA_DEP_UP;
}

#ifdef ENTELEA_TEST
void
entelea_assistant_test_reset(void)
{
	/* No module-level mutable state (the caller owns the window);
	 * this placeholder keeps the interface symmetric. */
}
#endif
