/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_ASSISTANT_H
#define ENTELEA_ASSISTANT_H

/* R08 minimal auxiliary-intelligence loop contract: see
 * framework/services/assistant/contract.md.
 * Event -> bounded context aggregation -> trigger -> structured candidate ->
 * permission approval -> task execution -> source-attributed feedback. The
 * model is an advisor: it cannot forge a principal, a source, or a capability.
 * Execution must pass the R04 permission gate and the R02 protection layer.
 * If the cloud is unavailable, intelligence is disabled (a standalone App still
 * runs). */

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

#define ENTELEA_CTX_EVENTS_MAX 16 /* bounded context window (suggested) */
#define ENTELEA_CANDIDATES_MAX 4 /* candidate limit per round (bounded) */
#define ENTELEA_SUGGESTION_TEXT_MAX 64

/* ---- Context aggregation (bounded window; evict oldest on overflow) ---- */

typedef struct {
	entelea_event_id_t id;
	uint64_t t_ms;
	const void *data;
	size_t len;
} entelea_ctx_event_t;

typedef struct {
	entelea_ctx_event_t events[ENTELEA_CTX_EVENTS_MAX];
	uint32_t count; /* entries in the current window */
	uint32_t dropped; /* overflow drop count */
} entelea_context_window_t;

/** [thread] Pushes an event into the window. When the window is full, the
 * oldest entry is evicted and dropped is incremented (bounded). */
void
entelea_ctx_push(entelea_context_window_t *w, const entelea_ctx_event_t *e);

/** Window snapshot (observation surface). */
void
entelea_ctx_snapshot(const entelea_context_window_t *w,
		     entelea_ctx_event_t *out, size_t max, size_t *n);

/** Overflow count. */
uint32_t
entelea_ctx_dropped(const entelea_context_window_t *w);

/* ---- Structured candidates (suggestions only, not execution permits) ---- */

typedef enum {
	ENTELEA_ACT_NONE = 0,
	ENTELEA_ACT_OUTPUT, /* output via protection layer (uid + value) */
	ENTELEA_ACT_TASK, /* task admission (R04) */
	ENTELEA_ACT_NOTIFY, /* notification only (no side effects) */
} entelea_action_kind_t;

typedef struct {
	entelea_action_kind_t kind;
	char text[ENTELEA_SUGGESTION_TEXT_MAX]; /* human-readable note */
	entelea_source_t source; /* suggestion source (model = MODEL) */
	const char *target_uid; /* ACT_OUTPUT target */
	uint8_t value_kind; /* 0=bool 1=u32 2=pwr */
	bool value_b;
	uint32_t value_u;
} entelea_candidate_t;

/* ---- Trigger (bounded; suggested rules registered in the contract) ---- */

/** [thread] Decides whether to trigger a deliberation round (window full,
 * timeout, or explicit request; suggested values). */
bool
entelea_should_deliberate(const entelea_context_window_t *w, uint64_t now_ms,
			  uint64_t *next_due_ms);

/* ---- Execution gate (candidate -> R04 permission -> R02 protection) ---- */

/** [thread] Executes a candidate. Without authorization, or with excess
 *  privilege, returns E_PERM (the model cannot approve itself). Output goes
 *  through entelea_safety_commit, the only write path of the protection layer.
 *  Returns the effect receipt. */
entelea_result_t
entelea_assistant_execute(entelea_principal_t actor,
			  const entelea_candidate_t *cand, int32_t *task_ret);

/* ---- Cloud dependency (D08: model down -> intelligence off) ---- */

/** Assistant availability: ENTELEA_DEP_MODEL is UP and the network is UP.
 * A standalone App does not check this surface. */
bool
entelea_assistant_available(void);

#ifdef ENTELEA_TEST
void
entelea_assistant_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_ASSISTANT_H */
