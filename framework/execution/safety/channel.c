/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Channel registry and three-safe-state state machine (migrated from the old
 * safety/channel.c; R02 contract: framework/execution/safety/contract.md).
 * A channel missing any of its three safe-state declarations is not
 * registered. Safety parameters are read-only at runtime (desc points to data
 * frozen at registration).
 * Changes from the old implementation (including review remediation):
 * - Control-source arbitration (set_arb/commit_src/ARB events) is deferred to
 *   R04; the audit ring is deferred to R03.
 * - Driver writes and shadow/state transitions all run inside the irq_lock
 *   critical section, so an estop ISR that preempts is not overwritten by
 *   updates made after the critical section (defect 3). SAFE_STATE_CHANGED is
 *   published after unlock, with forced re-checked before publishing (forced
 *   transitions from estop emit no per-channel events; the authoritative state
 *   is the channel_state/summary query).
 * - Driver write return values propagate: a failed poweron/link-loss/recover
 *   write returns E_IO explicitly and does not update the state as successful
 *   (defect 2). Activation transitions (-> ACTIVE) do not write the driver
 *   (recovery does not write back; DR-04).
 * - Module clock monotonic guard: on read failure or regression the last valid
 *   value is kept and counted (defect 5).
 */
#include <string.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include "internal.h"

struct entelea_ch_slot
	entelea_ch_table[ENTELEA_SAFETY_MAX_CHANNELS]; /* 32 slots (DEC-27) */
size_t entelea_ch_count;

atomic_t entelea_link_up = ATOMIC_INIT(0); /* Initially down until boot */
atomic_t entelea_arb_owner =
	ATOMIC_INIT(ENTELEA_ARB_REMOTE); /* Default remote; no local owner */
atomic_t entelea_forced = ATOMIC_INIT(0);
atomic_t entelea_forced_at = ATOMIC_INIT(0);
const entelea_clock_t *entelea_safety_clock;

/* Monotonic guard state: last valid clock value and failure count
 * (review defect 5). */
static uint64_t clock_last_good;
static uint32_t clock_fault_count;

/* Cumulative driver write failures (commit, poweron and transition failures
 * also propagate via return values; the force path cannot return from ISR
 * context, so this counter is the only trace). */
static uint32_t driver_write_failures;

/* Checked clock read: returns false = time unknown for this call (read
 * failure or regression; the guard intervened and *out holds the last
 * valid value). A regression must not pass the last valid value off as
 * known time, since that would borrow a time advanced by other channels
 * to set a slew bound (RF1 defect 4).
 * Concurrency (second-round review defect 5): commit/transitions (thread)
 * and force_all_fault/estop deferred publish (ISR) access the bookkeeping
 * concurrently; u64 can tear on 32-bit targets, so the compare, update
 * and count go inside an irq_lock micro critical section (ISR may nest). */
bool
entelea_safety_now_ms_checked(uint64_t *out)
{
	uint64_t ms = 0;
	unsigned int key = irq_lock();

	/* Read, compare and update all run under the lock (the read must be
	 * short and non-blocking). Thread and ISR sampling (force/estop
	 * deferred publish) is serialized, so normal progress is not
	 * misrecorded as a regression; a real regression counts as a fault
	 * and conservatively keeps the last valid value (the slew gate
	 * rejects on this basis). */
	bool ok = entelea_safety_clock != NULL &&
		  entelea_safety_clock->read_ms != NULL &&
		  entelea_safety_clock->read_ms(entelea_safety_clock->user,
						&ms) == ENTELEA_OK;

	if (!ok || ms < clock_last_good) {
		clock_fault_count++;
		ms = clock_last_good;
		ok = false;
	} else {
		clock_last_good = ms;
	}
	irq_unlock(key);
	*out = ms;
	return ok;
}

uint64_t
entelea_safety_now_ms(void)
{
	uint64_t v;

	(void) entelea_safety_now_ms_checked(&v);
	return v;
}

uint32_t
entelea_safety_clock_faults(void)
{
	return clock_fault_count;
}

uint32_t
entelea_safety_force_write_failures(void)
{
	return driver_write_failures;
}

/* Audit persist throttle (old F1/DEC-52①: no flash access under lock; a
 * failing commit loop would amplify anomalies into write/erase storms:
 * the first entry is persisted immediately, then 1 Hz sampling). */
#define ENTELEA_SAFETY_AUDIT_MIN_MS 1000
static uint32_t audit_last_persist_ms;
static uint32_t audit_throttled_count;

uint32_t
entelea_safety_audit_throttled(void)
{
	return audit_throttled_count;
}

void
entelea_safety_audit_persist(const void *entry, uint16_t evt)
{
	const entelea_storage_audit_entry_t *e = entry;
	uint32_t now = (uint32_t) entelea_safety_now_ms();

	if (audit_last_persist_ms != 0 &&
	    (uint32_t) (now - audit_last_persist_ms) <
		    ENTELEA_SAFETY_AUDIT_MIN_MS) {
		audit_throttled_count++;
		return;
	}
	audit_last_persist_ms = now;
	/* A persist failure does not feed back into the commit path (the
	 * observation surface degrades; memory and read surfaces stay
	 * observable). */
	(void) entelea_storage_audit_append(e, evt);
}

/** Record a driver write failure (called from commit and force paths). */
void
entelea_safety_count_write_failure(void)
{
	driver_write_failures++;
}

static bool
value_within_limits(const entelea_out_channel_t *ch, entelea_out_value_t v)
{
	switch (ch->kind) {
	case ENTELEA_CH_GPIO:
		return true;
	case ENTELEA_CH_PWM:
		return v.u >= ch->limits.min && v.u <= ch->limits.max;
	case ENTELEA_CH_POWER:
		if (!v.pwr.en) {
			return true;
		}
		return v.pwr.ma <= ch->limits.current_limit_ma;
	default:
		return false;
	}
}

entelea_result_t
entelea_safety_register_channel(const entelea_out_channel_t *ch)
{
	if (ch == NULL || ch->uid == NULL || ch->uid[0] == '\0' ||
	    (int) ch->kind < 0 || ch->kind >= ENTELEA_CH_KIND_COUNT) {
		return ENTELEA_E_PARAM;
	}
	if (entelea_ch_find(ch->uid) >= 0) {
		return ENTELEA_E_PARAM; /* uid already registered */
	}
	if (entelea_ch_count >= ENTELEA_SAFETY_MAX_CHANNELS) {
		return ENTELEA_E_NOMEM;
	}
	/* Reject if any of the three safe states is missing; each safe-state
	 * value must also lie within limits. */
	if (!value_within_limits(ch, ch->poweron) ||
	    !value_within_limits(ch, ch->linkloss) ||
	    !value_within_limits(ch, ch->fault)) {
		return ENTELEA_E_PARAM;
	}

	struct entelea_ch_slot *slot = &entelea_ch_table[entelea_ch_count++];

	slot->desc = ch;
	slot->state = ENTELEA_ST_SAFE_POWERON;
	slot->shadow = ch->poweron;
	slot->have_last = false;
	slot->last_t = 0;
	slot->sim_rec.head = 0;
	slot->sim_rec.count = 0;
	return ENTELEA_OK;
}

entelea_result_t
entelea_safety_bind_clock(const entelea_clock_t *clock)
{
	if (clock == NULL) {
		entelea_safety_clock = NULL; /* Restore for test isolation */
		return ENTELEA_OK;
	}
	if (clock->read_ms == NULL || clock->clock_id == 0) {
		return ENTELEA_E_PARAM;
	}
	entelea_safety_clock = clock;
	return ENTELEA_OK;
}

/* Publish to running subscriber callbacks: only outside the critical
 * section, and only after re-checking that forced is clear (no stale
 * transition event is published inside a forced window). */
static void
publish_state_change(size_t i, entelea_ch_state_t next)
{
	const struct entelea_ch_slot *s = &entelea_ch_table[i];
	const entelea_safe_state_evt_t payload = {
		.uid = s->desc->uid,
		.new_state = (int32_t) next,
	};
	const entelea_event_t evt = {
		.id = ENTELEA_EVT_SAFE_STATE_CHANGED,
		.data = &payload,
		.len = sizeof(payload),
	};

	entelea_evt_publish(&evt);
}

/** Write driver + shadow/state transition; caller must hold irq_lock (this
 * closes the window in which estop can preempt).
 * Returns 0 = done; 1 = preempted by forced (stays SAFE_FAULT); -1 = backend
 * write failure.
 * A2 (owner-approved 2026-11-08): a safe-state write failure means the
 * physical effect is unknown, so the channel enters SAFE_FAULT (output
 * eligibility revoked; later commits are rejected; recover restores it
 * explicitly). The shadow keeps the last confirmed value (a safe value is
 * never claimed to have reached the driver); recovery must rewrite via
 * recover. */
static int
write_transition_locked(struct entelea_ch_slot *s, entelea_out_value_t value,
			entelea_ch_state_t next)
{
	if (atomic_get(&entelea_forced) != 0) {
		return 1;
	}
	if (entelea_drivers[s->desc->kind].write(s->desc, &value) != 0) {
		driver_write_failures++;
		s->state = ENTELEA_ST_SAFE_FAULT;
		s->fault_by_write_failure =
			true; /* Only channel_recover can recover */
		/* Baseline kept: a failed write is not a confirmed write; the
		 * anchor stays at the last confirmed value and time. */
		return -1;
	}
	s->shadow = value;
	s->fault_by_write_failure = false;
	/* A confirmed write refreshes the slew anchor (decision C): safe-state
	 * transitions no longer reset the baseline, so output always ramps
	 * from the last confirmed value; estop-clear cycles no longer produce
	 * a free jump. now_ms may nest irq_lock internally. */
	s->last_t = entelea_safety_now_ms();
	s->have_last = true;
	s->state = next;
	return 0;
}

/** State transition only (activation path: no driver write; recovery does
 * not write back, DR-04). Caller must hold irq_lock.
 * Baseline unchanged (activation performs no physical write, so the last
 * confirmed value remains valid). Returns 0 = done; 1 = forced. */
static int
activate_locked(struct entelea_ch_slot *s)
{
	if (atomic_get(&entelea_forced) != 0) {
		return 1;
	}
	s->state = ENTELEA_ST_ACTIVE;
	return 0;
}

entelea_result_t
entelea_safety_poweron_init(void)
{
	bool any_fail = false;

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		/* Write the registration-time poweron value to the driver: at
		 * boot step 2 the physical output reaches the declared
		 * power-on safe state. estop may preempt at any time after
		 * estop_init (step 1): if forced is set, skip and keep
		 * the ISR-set SAFE_FAULT (defect 3). Write failures are
		 * aggregated and reported -> boot fail-safe (defect 2; the
		 * unknown physical state is closed by fail-safe). */
		unsigned int key = irq_lock();
		int rc = write_transition_locked(s, s->desc->poweron,
						 ENTELEA_ST_SAFE_POWERON);

		irq_unlock(key);
		if (rc < 0) {
			any_fail = true;
		}
	}
	return any_fail ? ENTELEA_E_IO : ENTELEA_OK;
}

entelea_result_t
entelea_safety_set_link(bool up)
{
	bool any_fail = false;

	/* Serialize on write_lock: transition paths exclude commit and
	 * single-channel transitions (thread context). */
	entelea_safety_write_lock();
	atomic_set(&entelea_link_up, up ? 1 : 0);
	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (up && (s->state == ENTELEA_ST_SAFE_POWERON ||
			   s->state == ENTELEA_ST_SAFE_LINKLOSS)) {
			/* Activation: no driver write (recovery does
			 * not rewrite the pre-link-loss value). */
			unsigned int key = irq_lock();
			int rc = activate_locked(s);

			irq_unlock(key);
			if (rc == 0 && atomic_get(&entelea_forced) == 0) {
				publish_state_change(i, ENTELEA_ST_ACTIVE);
			}
		} else if (!up && s->state == ENTELEA_ST_ACTIVE) {
			/* Link loss: write the declared value and transition,
			 * all inside the critical section (defect 3). */
			unsigned int key = irq_lock();
			int rc = write_transition_locked(
				s, s->desc->linkloss, ENTELEA_ST_SAFE_LINKLOSS);

			irq_unlock(key);
			if (rc < 0) {
				any_fail = true;
			} else if (rc == 0 &&
				   atomic_get(&entelea_forced) == 0) {
				publish_state_change(i,
						     ENTELEA_ST_SAFE_LINKLOSS);
			}
		}
	}
	entelea_safety_write_unlock();
	return any_fail ? ENTELEA_E_IO : ENTELEA_OK;
}

entelea_result_t
entelea_safety_set_arb(entelea_arb_owner_t owner)
{
	if (owner != ENTELEA_ARB_REMOTE && owner != ENTELEA_ARB_LOCAL) {
		return ENTELEA_E_PARAM;
	}
	if (k_is_in_isr()) {
		return ENTELEA_E_PERM; /* [thread] API (events via bus) */
	}
	entelea_arb_owner_t old =
		(entelea_arb_owner_t) atomic_get(&entelea_arb_owner);

	if (old == owner) {
		return ENTELEA_OK; /* Idempotent: no events, no output */
	}
	atomic_set(&entelea_arb_owner, owner);
	/* Switching emits no output (output continuity is the APP's job);
	 * only the gate bit and a logged event change. */
	const entelea_arb_evt_t payload = {
		.owner = (uint8_t) owner,
		.old = (uint8_t) old,
	};
	const entelea_event_t evt = {
		.id = ENTELEA_EVT_ARB_CHANGED,
		.data = &payload,
		.len = sizeof(payload),
	};
	entelea_evt_publish(&evt);
	return ENTELEA_OK;
}

entelea_arb_owner_t
entelea_safety_arb_owner(void)
{
	return (entelea_arb_owner_t) atomic_get(&entelea_arb_owner);
}

#ifdef ENTELEA_TEST
void
entelea_safety_test_reset(void)
{
	memset(entelea_ch_table, 0, sizeof(entelea_ch_table));
	entelea_ch_count = 0;
	atomic_set(&entelea_forced, 0);
	atomic_set(&entelea_forced_at, 0);
	atomic_set(&entelea_link_up, 0);
	atomic_set(&entelea_arb_owner, ENTELEA_ARB_REMOTE);
	clock_last_good = 0;
	clock_fault_count = 0;
	driver_write_failures = 0;
	audit_last_persist_ms = 0;
	audit_throttled_count = 0;
	entelea_drv_test_reset();
	entelea_safety_test_reset_system_failed();
	entelea_safety_estop_announce_reset();
}
#endif

entelea_result_t
entelea_safety_force_channel_fault(const char *uid)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	/* Serialize with commit and transitions */
	entelea_safety_write_lock();
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	entelea_ch_state_t prev = s->state;
	unsigned int key = irq_lock();
	int rc = write_transition_locked(s, s->desc->fault,
					 ENTELEA_ST_SAFE_FAULT);

	irq_unlock(key);
	entelea_safety_write_unlock();
	if (rc < 0) {
		return ENTELEA_E_IO; /* Backend failed; state unchanged */
	}
	if (rc == 0 && prev != ENTELEA_ST_SAFE_FAULT &&
	    atomic_get(&entelea_forced) == 0) {
		publish_state_change(i, ENTELEA_ST_SAFE_FAULT);
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_safety_channel_recover(const char *uid)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	if (atomic_get(&entelea_forced) != 0) {
		return ENTELEA_E_STATE; /* Latched by estop or system_fail */
	}
	/* Serialize with commit and transitions */
	entelea_safety_write_lock();
	if (atomic_get(&entelea_forced) != 0) {
		entelea_safety_write_unlock();
		return ENTELEA_E_STATE; /* Double-check: recheck under lock */
	}
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	entelea_ch_state_t next = atomic_get(&entelea_link_up) ?
					  ENTELEA_ST_ACTIVE :
					  ENTELEA_ST_SAFE_POWERON;

	/* Physical reattach = power-on semantics: write the poweron value,
	 * with write and transition inside the critical section (defect 3). */
	unsigned int key = irq_lock();
	int rc = write_transition_locked(s, s->desc->poweron, next);

	irq_unlock(key);
	entelea_safety_write_unlock();
	if (rc < 0) {
		return ENTELEA_E_IO;
	}
	if (rc == 0 && atomic_get(&entelea_forced) == 0) {
		publish_state_change(i, next);
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_safety_clear_fault(void)
{
	/* The only release path for the estop latch (runtime reachability
	 * belongs to the later command surface; direct R02 calls are for
	 * tests only). Release semantics: state recovers conditionally
	 * (link up -> ACTIVE, otherwise SAFE_LINKLOSS), and output values
	 * are not written back; the physical output keeps the fault safe
	 * value until the next explicit commit. estop may preempt again at
	 * any time after release: each channel re-checks forced inside the
	 * critical section (defect 3).
	 * Release eligibility (third-round review finding 1): system_fail is
	 * terminal (one-shot halt, halt semantics), so clear_fault rejects
	 * after it; channels in SAFE_FAULT due to a safe-state write failure
	 * (physical state unknown) are not recovered on this path, only by
	 * an explicit channel_recover. */
	if (atomic_get(&entelea_system_failed) != 0) {
		return ENTELEA_E_STATE; /* No recovery after system_fail */
	}
	atomic_set(&entelea_forced, 0);
	entelea_safety_estop_announce_reset();
	/* Serialize batch state transitions with commit and
	 * single-channel transitions. */
	entelea_safety_write_lock();
	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (s->state != ENTELEA_ST_SAFE_FAULT ||
		    s->fault_by_write_failure) {
			continue; /* Write-failed channels: recover only */
		}
		entelea_ch_state_t next = atomic_get(&entelea_link_up) ?
						  ENTELEA_ST_ACTIVE :
						  ENTELEA_ST_SAFE_LINKLOSS;
		unsigned int key = irq_lock();

		if (atomic_get(&entelea_forced) == 0) {
			s->state = next; /* No write, baseline kept (dec. C) */
		}
		irq_unlock(key);
		if (atomic_get(&entelea_forced) == 0) {
			publish_state_change(i, next);
		}
	}
	entelea_safety_write_unlock();
	return ENTELEA_OK;
}

entelea_result_t
entelea_safety_channel_state(const char *uid, entelea_ch_state_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	*out = entelea_ch_table[i].state;
	return ENTELEA_OK;
}

entelea_result_t
entelea_safety_summary(entelea_safety_summary_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if ((int) entelea_ch_table[i].state >= 0 &&
		    entelea_ch_table[i].state < ENTELEA_CH_STATE_COUNT) {
			out->by_state[entelea_ch_table[i].state]++;
		}
	}
	out->channels = (uint16_t) entelea_ch_count;
	return ENTELEA_OK;
}
