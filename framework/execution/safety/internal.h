/* SPDX-License-Identifier: Apache-2.0 */
/* entelea safety internal state (not exported): channel slot table,
 * forced/link atomics, and lock consolidation. */
#ifndef ENTELEA_SAFETY_INTERNAL_H
#define ENTELEA_SAFETY_INTERNAL_H

#include <string.h>
#include <entelea/safety.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct entelea_ch_slot {
	const entelea_out_channel_t *desc;
	entelea_ch_state_t state;
	entelea_out_value_t shadow; /* Last confirmed value; kept on failure */
	bool have_last; /* Whether slew baseline is established */
	uint64_t last_t; /* Time of last confirmed write (slew dt anchor) */
	bool fault_by_write_failure; /* SAFE_FAULT from a failed write:
				      * only channel_recover can recover;
				      * clear_fault skips it. Physical effect
				      * unknown */
	struct {
		uint32_t head, count;
		entelea_write_rec_t ring[ENTELEA_SAFETY_SIM_REC_PER_CH];
	} sim_rec; /* Write sequence record (golden data source) */
};

extern struct entelea_ch_slot entelea_ch_table[ENTELEA_SAFETY_MAX_CHANNELS];
extern size_t entelea_ch_count;

extern atomic_t entelea_forced; /* estop/fail latch (cleared by clear_fault) */
extern atomic_t
	entelea_system_failed; /* system_fail occurred; clear_fault refuses */
extern atomic_t entelea_forced_at; /* forced set time (low 32 bits) */
extern atomic_t entelea_link_up; /* Global link flag */
extern atomic_t
	entelea_arb_owner; /* Control-source seat (R04; entelea_arb_owner_t) */
extern const entelea_clock_t *entelea_safety_clock;

/* Module clock read (implemented in channel.c): on read failure or
 * regression the last valid value is kept (monotonic guard: a failure must
 * not fake 0, which would underflow the slew subtraction and loosen the
 * limit). Failures are counted in clock_faults; returns 0 if it never
 * succeeded. The checked variant returns false = time unknown for this call
 * (the slew gate and callers reject time-bounded operations on that basis). */
uint64_t
entelea_safety_now_ms(void);
bool
entelea_safety_now_ms_checked(uint64_t *out);

/* Module-private driver ops table (defined in drivers.c; references from
 * outside the module are rejected by the architecture check). */
typedef struct {
	int (*write)(const entelea_out_channel_t *ch,
		     const entelea_out_value_t *v);
	int (*read)(const entelea_out_channel_t *ch, entelea_out_value_t *out);
} entelea_driver_ops_t;

extern const entelea_driver_ops_t entelea_drivers[ENTELEA_CH_KIND_COUNT];

/** Count a driver write failure (the counter lives in channel.c; called
 * from commit and force paths). */
void
entelea_safety_count_write_failure(void);

/** Audit persist (channel.c): exception-class throttling (first entry
 * immediate, then 1 Hz sampling; old DEC-52① semantics; drop counts during
 * storms remain observable). A persist failure does not feed back into the
 * commit path (observation surface degrades). */
void
entelea_safety_audit_persist(const void *entry, uint16_t evt);

#ifdef ENTELEA_TEST
/** Reset backend-ready flags and failure counters (drivers.c; test
 * isolation). */
void
entelea_drv_test_reset(void);
/** Reset the one-shot system_failed flag (force.c; test isolation only). */
void
entelea_safety_test_reset_system_failed(void);
#endif

/** Reset the estop-announced flag (called by clear_fault; a second estop
 * can announce again). */
void
entelea_safety_estop_announce_reset(void);

/* ---- Lock consolidation (old DEC-43 option A semantics) -----------------
 * write_lock is the controlled exposure of commit_lock: safe-state transition
 * paths (set_link / single-channel force/recover / clear_fault) must be
 * mutually exclusive with commit. Thread context only (ISR entry forbidden);
 * a caller that already holds the lock must call commit_locked (k_mutex is
 * not recursive).
 * Lock order: write_lock -> event publish path, one direction, no cycles.
 * R04 budget check-and-commit atomicity reuses this lock; R02 has no external
 * holders. */
void
entelea_safety_write_lock(void);
void
entelea_safety_write_unlock(void);

/* Find slot index; returns -1 if not registered */
static inline int
entelea_ch_find(const char *uid)
{
	if (uid == NULL) {
		return -1;
	}
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if (entelea_ch_table[i].desc != NULL &&
		    entelea_ch_table[i].desc->uid != NULL &&
		    strcmp(entelea_ch_table[i].desc->uid, uid) == 0) {
			return (int) i;
		}
	}
	return -1;
}

#endif /* ENTELEA_SAFETY_INTERNAL_H */
