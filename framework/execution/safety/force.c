/* SPDX-License-Identifier: Apache-2.0 */
/*
 * estop / fail-safe direct path (migrated from the old safety/force.c; R02
 * contract: framework/execution/safety/contract.md).
 * estop path discipline: the force_all_fault call graph must not contain
 * allocation, queue, lock, or protocol stack symbols; only atomic set plus
 * direct driver writes (same discipline as drivers.c).
 * Changes from the old implementation: persisting the noinit trace is deferred
 * to R03 (R02 only exposes the in-memory reason); writing estop triggers into
 * the flash audit ring is also deferred to R03.
 */
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/wdt.h>
#include "internal.h"

static uint32_t fail_reason_store;
atomic_t entelea_system_failed = ATOMIC_INIT(0);

/* Reset-reason trace hook: the strong implementation in the storage module
 * (R03) writes to noinit; builds without storage get a weak stub (a missing
 * trace is reported as missing, never faked). */
__weak void
entelea_safety_record_fail_reason(uint32_t reason)
{
	(void) reason;
}

#ifdef ENTELEA_TEST
void
entelea_safety_test_reset_system_failed(void)
{
	atomic_set(&entelea_system_failed, 0);
	fail_reason_store = 0; /* Plan-conformance review fix: reason had no
				* reset path; stale values from earlier
				* cases leaked into later fail_reason
				* assertions (test-isolation gap). */
}
#endif

/* estop ISR direct path: [ISR]-safe; only atomic set and direct writes
 * (no queue or scheduling). A write failure cannot return in ISR context,
 * so it is counted (observable); state and shadow still transition per the
 * contract (unknown physical state is closed out by fail-safe/WDT). */
void
entelea_safety_force_all_fault(void)
{
	bool any_write_failed = false;

	atomic_set(&entelea_forced, 1);
	atomic_set(&entelea_forced_at,
		   (atomic_val_t) (uint32_t) entelea_safety_now_ms());

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (entelea_drivers[s->desc->kind].write(
			    s->desc, &s->desc->fault) != 0) {
			/* Write failed: physical effect unknown.
			 * Do not claim the fault value landed; the shadow
			 * keeps the last confirmed value. State still revokes
			 * eligibility and is marked recover-only
			 * (third-round review finding 2). */
			entelea_safety_count_write_failure();
			s->state = ENTELEA_ST_SAFE_FAULT;
			s->fault_by_write_failure = true;
			/* Baseline kept: no confirmed write (decision C). */
			any_write_failed = true;
			continue;
		}
		s->shadow = s->desc->fault; /* Confirmed: clears unknown flag */
		s->fault_by_write_failure = false;
		s->last_t = entelea_safety_now_ms(); /* Refresh anchor */
		s->have_last = true;
		s->state = ENTELEA_ST_SAFE_FAULT;
	}
	if (any_write_failed) {
		/* Escalate to global halt (owner-approved 2026-11-08, item 1):
		 * if the estop forced write to the driver fails, the output
		 * cannot be guaranteed to reach its safe value. halt is an
		 * atomic set (ISR-safe); after feeding stops, the WDT reset
		 * closes the case. Remaining channels are made safe first,
		 * then escalate.
		 * A halt is terminal (RF1 defect 3): system_failed is set
		 * together with system_fail, and clear_fault/recover must not
		 * restore any channel output before the WDT reset. */
		atomic_set(&entelea_system_failed, 1);
		entelea_wdt_halt();
	}
	/* Deferred ENTELEA_EVT_ESTOP publish: a sysworkq periodic item
	 * publishes after detecting the forced rising edge (ISR must not
	 * submit work items; queue ban). Response bound = patrol period
	 * + scheduling delay. */
}

bool
entelea_safety_estop_latched(void)
{
	return atomic_get(&entelea_forced) != 0;
}

uint32_t
entelea_safety_estop_latched_at(void)
{
	return (uint32_t) atomic_get(&entelea_forced_at);
}

void
entelea_safety_system_fail(uint32_t reason)
{
	fail_reason_store = reason;
	/* Global halt first (defect 4): if any thread triggers system_fail,
	 * the soft WDT patrol immediately stops feeding and rescheduling; halt
	 * semantics do not depend on the calling thread suspending itself. */
	entelea_wdt_halt();
	atomic_set(&entelea_system_failed,
		   1); /* One-shot: clear_fault refuses release */
	atomic_set(&entelea_forced, 1);
	atomic_set(&entelea_forced_at,
		   (atomic_val_t) (uint32_t) entelea_safety_now_ms());

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (entelea_drivers[s->desc->kind].write(
			    s->desc, &s->desc->fault) != 0) {
			entelea_safety_count_write_failure();
			s->state = ENTELEA_ST_SAFE_FAULT;
			s->fault_by_write_failure = true;
			/* Baseline kept: no confirmed write (decision C). */
			continue;
		}
		s->shadow = s->desc->fault; /* Confirmed: clears unknown flag */
		s->fault_by_write_failure = false;
		s->last_t = entelea_safety_now_ms(); /* Refresh anchor */
		s->have_last = true;
		s->state = ENTELEA_ST_SAFE_FAULT;
	}
	/* Reset-reason trace (R03 wiring: the strong storage implementation
	 * writes noinit; the weak stub means a build without storage). */
	entelea_safety_record_fail_reason(reason);

#ifdef ENTELEA_TEST
	return; /* Test build: return here; the test checks the halt path */
#endif
	/* Controlled halt: stop feeding the hard WDT; the fallback reset
	 * (reset-reason trace belongs to R03). */
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

#ifdef ENTELEA_TEST
uint32_t
entelea_safety_test_fail_reason(void)
{
	return fail_reason_store;
}
#endif

/* Deferred estop publish (sysworkq periodic; the wdt patrol reuses the
 * same cadence) */
static bool estop_announced;

void
entelea_safety_estop_announce_reset(void)
{
	estop_announced =
		false; /* clear_fault resets it; a second estop re-announces */
}

void
entelea_safety_estop_deferred_publish(void)
{
	if (atomic_get(&entelea_forced) != 0 && !estop_announced) {
		estop_announced = true;
		const entelea_estop_evt_t payload = {
			.t_ms = (uint32_t) atomic_get(&entelea_forced_at),
		};
		const entelea_event_t evt = {
			.id = ENTELEA_EVT_ESTOP,
			.data = &payload,
			.len = sizeof(payload),
		};
		entelea_evt_publish(&evt);
		/* Estop trigger goes to the audit ring (low-frequency edge,
		 * not throttled; evt=1). */
		const entelea_storage_audit_entry_t au = {
			.t_ms = payload.t_ms,
			.ch_idx = 0xFFFF,
			.res = ENTELEA_E_STATE,
			.evt = 1,
		};

		(void) entelea_storage_audit_append(&au, 1);
	}
}
