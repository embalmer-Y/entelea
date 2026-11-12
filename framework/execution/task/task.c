/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Task state machine (new R04 implementation; contract:
 * framework/execution/task/contract.md; N-TASK, new-only). Bounded table,
 * admission deduplication, cancellation effect field, and terminal-state
 * reclamation. accepted != completed; cancellation does not undo effects
 * that already happened; unknown (non-idempotent) effects must not be
 * retried automatically.
 * Locking: a single mutex serializes submit, start, terminal transitions
 * and reclaim; queries are lock-free (observers tolerate micro-tearing, and
 * the authoritative state is read through the same query surface). No
 * model: the test principal drives the closed loop directly.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/task.h>

static struct {
	entelea_task_t tasks[ENTELEA_TASK_MAX];
	bool used[ENTELEA_TASK_MAX];
	uint16_t next_seq;
} tb;
static struct k_mutex task_lock = Z_MUTEX_INITIALIZER(task_lock);
static const entelea_clock_t *task_clock;
static uint64_t task_last_ms;
static bool task_has_last;

entelea_result_t
entelea_task_bind_clock(const entelea_clock_t *clock)
{
	if (clock == NULL || clock->read_ms == NULL || clock->clock_id == 0) {
		return ENTELEA_E_PARAM;
	}
	k_mutex_lock(&task_lock, K_FOREVER);
	if (task_clock != NULL && task_clock->clock_id != clock->clock_id) {
		for (int i = 0; i < ENTELEA_TASK_MAX; i++) {
			if (tb.used[i] && tb.tasks[i].deadline_ms != 0 &&
			    (tb.tasks[i].state == ENTELEA_TASK_ACCEPTED ||
			     tb.tasks[i].state == ENTELEA_TASK_RUNNING)) {
				k_mutex_unlock(&task_lock);
				return ENTELEA_E_BUSY;
			}
		}
		task_has_last = false;
	}
	task_clock = clock;
	k_mutex_unlock(&task_lock);
	return ENTELEA_OK;
}

/* Caller holds task_lock; a backward step is not reliable evidence for
 * a deadline. */
static bool
task_now_ms(uint64_t *out)
{
	uint64_t now;

	if (task_clock == NULL ||
	    task_clock->read_ms(task_clock->user, &now) != ENTELEA_OK ||
	    (task_has_last && now < task_last_ms)) {
		return false;
	}
	task_last_ms = now;
	task_has_last = true;
	*out = now;
	return true;
}

/* Lazy expiry (requires task_lock): ACCEPTED past its deadline -> CANCELLED
 * with effect NONE (not started, so no side effects). RUNNING does not
 * transition (an unknown effect is not decided on its behalf). */
static bool
expire_locked(entelea_task_t *t)
{
	if (t->deadline_ms == 0 || t->state != ENTELEA_TASK_ACCEPTED) {
		return true;
	}
	uint64_t now;

	if (!task_now_ms(&now)) {
		return false;
	}
	if (now < t->deadline_ms) {
		return true;
	}
	t->state = ENTELEA_TASK_CANCELLED;
	t->effect = ENTELEA_EFFECT_NONE;
	return true;
}

static int
find_id(entelea_task_id_t id)
{
	if (id == 0 || id > ENTELEA_TASK_MAX) {
		return -1;
	}
	return tb.used[id - 1] ? (int) (id - 1) : -1;
}

static bool
same_principal(entelea_principal_t a, entelea_principal_t b)
{
	return a._handle == b._handle && a._gen == b._gen &&
	       a.app_id == b.app_id;
}

entelea_result_t
entelea_task_submit(entelea_principal_t ctx, const char *name, uint16_t seq,
		    uint64_t deadline_ms, entelea_task_id_t *out)
{
	if (name == NULL) {
		return ENTELEA_E_PARAM;
	}
	/* out is written only on success (failure does not clear it), so a
	 * task id the caller already holds survives duplicate rejection. */
	k_mutex_lock(&task_lock, K_FOREVER);
	/* Deadline admission gate and clock bookkeeping share task_lock. */
	if (deadline_ms != 0) {
		uint64_t now;

		if (!task_now_ms(&now)) {
			k_mutex_unlock(&task_lock);
			return ENTELEA_E_STATE;
		}
		if (now >= deadline_ms) {
			k_mutex_unlock(&task_lock);
			return ENTELEA_E_RANGE; /* Expired on admit: reject */
		}
	}
	/* Admission dedup: a non-terminal task with the same principal and
	 * seq returns E_BUSY (duplicate requests are detectable and never
	 * merged silently; restart-retry semantics are up to the caller,
	 * who uses a new seq). */
	for (int i = 0; i < ENTELEA_TASK_MAX; i++) {
		if (tb.used[i]) {
			expire_locked(&tb.tasks[i]); /* Frees dedup slot */
		}
		if (tb.used[i] && same_principal(tb.tasks[i].owner, ctx) &&
		    tb.tasks[i].seq == seq &&
		    (tb.tasks[i].state == ENTELEA_TASK_ACCEPTED ||
		     tb.tasks[i].state == ENTELEA_TASK_RUNNING)) {
			k_mutex_unlock(&task_lock);
			return ENTELEA_E_BUSY;
		}
	}
	for (int i = 0; i < ENTELEA_TASK_MAX; i++) {
		if (!tb.used[i]) {
			tb.used[i] = true;
			tb.tasks[i].id = (entelea_task_id_t) (i + 1);
			tb.tasks[i].state = ENTELEA_TASK_ACCEPTED;
			tb.tasks[i].effect = ENTELEA_EFFECT_NONE;
			tb.tasks[i].owner = ctx;
			tb.tasks[i].seq = seq;
			tb.tasks[i].deadline_ms = deadline_ms;
			strncpy(tb.tasks[i].name, name,
				ENTELEA_TASK_NAME_MAX - 1);
			tb.tasks[i].name[ENTELEA_TASK_NAME_MAX - 1] = '\0';
			if (out != NULL) {
				*out = tb.tasks[i].id;
			}
			k_mutex_unlock(&task_lock);
			return ENTELEA_OK;
		}
	}
	k_mutex_unlock(&task_lock);
	return ENTELEA_E_NOMEM; /* Slots full (bounded; no queueing) */
}

static entelea_result_t
transition_effect(entelea_principal_t ctx, entelea_task_id_t id,
		  entelea_task_state_t expect_from, entelea_task_state_t next,
		  bool flag)
{
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	expire_locked(t); /* Lazy expiry first (expired -> E_STATE) */
	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM; /* Owned by another principal */
	}
	if (t->state != expect_from) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_STATE; /* State race: explicit reject */
	}
	t->state = next;
	/* The effect field is written under the lock (review P1 fix: a write
	 * after unlock could race with release+reuse and overwrite a new
	 * task, so confirmation could be forged). */
	if (next == ENTELEA_TASK_SUCCEEDED) {
		t->effect =
			flag ? ENTELEA_EFFECT_CONFIRMED : ENTELEA_EFFECT_NONE;
	} else {
		t->effect = flag ? ENTELEA_EFFECT_UNKNOWN : ENTELEA_EFFECT_NONE;
	}
	k_mutex_unlock(&task_lock);
	return ENTELEA_OK;
}

static entelea_result_t
transition(entelea_principal_t ctx, entelea_task_id_t id,
	   entelea_task_state_t expect_from, entelea_task_state_t next)
{
	/* State transition without an effect field (used for intermediate
	 * transitions such as start). */
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM;
	}
	if (t->state != expect_from || !expire_locked(t) ||
	    t->state != expect_from) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_STATE;
	}
	t->state = next;
	k_mutex_unlock(&task_lock);
	return ENTELEA_OK;
}

entelea_result_t
entelea_task_start(entelea_principal_t ctx, entelea_task_id_t id)
{
	return transition(ctx, id, ENTELEA_TASK_ACCEPTED, ENTELEA_TASK_RUNNING);
}

static entelea_result_t
finish(entelea_principal_t ctx, entelea_task_id_t id, entelea_task_state_t st,
       bool flag)
{
	/* The caller supplies the effect field from the real receipt; this
	 * layer never fakes confirmation. The write happens under the lock. */
	return transition_effect(ctx, id, ENTELEA_TASK_RUNNING, st, flag);
}

entelea_result_t
entelea_task_succeed(entelea_principal_t ctx, entelea_task_id_t id,
		     bool effect_confirmed)
{
	return finish(ctx, id, ENTELEA_TASK_SUCCEEDED, effect_confirmed);
}

entelea_result_t
entelea_task_fail(entelea_principal_t ctx, entelea_task_id_t id,
		  bool effect_unknown)
{
	return finish(ctx, id, ENTELEA_TASK_FAILED, effect_unknown);
}

entelea_result_t
entelea_task_cancel(entelea_principal_t ctx, entelea_task_id_t id)
{
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	expire_locked(t); /* Expired is CANCELLED: rejected below */
	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM;
	}
	if (t->state == ENTELEA_TASK_ACCEPTED) {
		t->state = ENTELEA_TASK_CANCELLED;
		t->effect = ENTELEA_EFFECT_NONE; /* Not started: no effect */
		k_mutex_unlock(&task_lock);
		return ENTELEA_OK;
	}
	if (t->state == ENTELEA_TASK_RUNNING) {
		t->state = ENTELEA_TASK_CANCELLED;
		t->effect = ENTELEA_EFFECT_UNKNOWN; /* Effects are not undone */
		k_mutex_unlock(&task_lock);
		return ENTELEA_OK;
	}
	k_mutex_unlock(&task_lock);
	return ENTELEA_E_STATE; /* Cancel after terminal state */
}

entelea_result_t
entelea_task_get(entelea_task_id_t id, entelea_task_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	int i = find_id(id);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	*out = tb.tasks[i]; /* Lock-free snapshot (micro-tearing tolerated) */
	return ENTELEA_OK;
}

entelea_result_t
entelea_task_release(entelea_principal_t ctx, entelea_task_id_t id)
{
	k_mutex_lock(&task_lock, K_FOREVER);
	int i = find_id(id);

	if (i < 0) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_NOTFOUND;
	}
	entelea_task_t *t = &tb.tasks[i];

	if (!same_principal(t->owner, ctx)) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_PERM;
	}
	expire_locked(t); /* Expired ACCEPTED becomes terminal first */
	if (t->state == ENTELEA_TASK_ACCEPTED ||
	    t->state == ENTELEA_TASK_RUNNING) {
		k_mutex_unlock(&task_lock);
		return ENTELEA_E_STATE; /* Non-terminal: cannot reclaim */
	}
	tb.used[i] = false;
	memset(t, 0, sizeof(*t));
	k_mutex_unlock(&task_lock);
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void
entelea_task_test_reset(void)
{
	memset(&tb, 0, sizeof(tb));
	task_clock = NULL; /* Also isolates clock binding (tests rebind) */
	task_last_ms = 0;
	task_has_last = false;
}
#endif
