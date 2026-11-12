/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_D12_H
#define ENTELEA_D12_H

/* D12 product threshold runtime gate (owner-approved v1 conservative plan,
 * 2026-11-10; see docs/d12-threshold-proposal.md). Three surfaces:
 *   cooldown: 60s cooldown after E_BUSY; 60s business cooldown after an
 *             abnormal reset
 *   frequency: rolling 60s window of at most 6 calls; UTC daily quota of at
 *             most 200 calls (persistent across resets and power loss;
 *             reserve-style writes, see d12.c)
 *   storm: latch after 3 consecutive failures (not cleared automatically;
 *          released by the external storm_clear command; rebinding the
 *          clock does not release it)
 * The counting unit is one transport request (model_ask retries each pass the
 * gate and are reported each time; RF1 G5). Decisions are purely synchronous
 * (no timers): callers ask with *_allow first and report with *_note
 * afterwards. Time is clamped monotonically: on a backward step, the last
 * valid value is kept, so windows and cooldowns may only extend, never
 * shrink. If the clock is blind when a cooldown is reported, the cooldown is
 * suspended and starts at the first good reading (nothing is lost). Day
 * boundaries use the UTC wall clock; a backward date change does not relax
 * the limit. An unknown wall clock, a failed persistent read or write, or a
 * corrupted record is refused (E_STATE), never allowed with empty state.
 * Entry is serialized through the mutex port: an unbound lock refuses, and
 * ISRs must not call this. */

#include <stdbool.h>
#include <stdint.h>
#include <entelea/time.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Daily quota and persistent-record parameters. K is the reserve step: each
 * time, the persisted limit is raised K calls ahead. Daily writes are about
 * MAX/K (plus the first reservation after a day change or restart). */
#define ENTELEA_D12_DAILY_MAX 200U
#define ENTELEA_D12_RESERVE_STEP 10U
#define ENTELEA_D12_REC_LEN 12U

/** Wall-clock port (day boundary): reads UTC seconds. Returns E_STATE if the
 * clock is not synchronized (the gate fails closed). */
typedef struct {
	entelea_result_t (*read_utc_s)(void *user, uint64_t *out);
	void *user;
} entelea_wallclock_t;

/** Mutex port: non-recursive. In ISR context, lock must return E_STATE. */
typedef struct {
	entelea_result_t (*lock)(void *user);
	void (*unlock)(void *user);
	void *user;
} entelea_lock_t;

/** Daily-count persistent port: each record is a fixed ENTELEA_D12_REC_LEN
 * bytes (encoding in d12.c). load returns E_NOTFOUND if there is no record;
 * any other error makes the gate refuse; it is not treated as empty. */
typedef struct {
	entelea_result_t (*load)(void *user, uint8_t rec[ENTELEA_D12_REC_LEN]);
	entelea_result_t (*save)(void *user,
				 const uint8_t rec[ENTELEA_D12_REC_LEN]);
	void *user;
} entelea_d12_store_t;

/** Binds the clock (the decision dependency). */
entelea_result_t
entelea_d12_bind_clock(const entelea_clock_t *clock);
/** Binds the wall clock, mutex, and persistent ports (called at startup; an
 * unbound port makes decisions refuse). */
entelea_result_t
entelea_d12_bind_wallclock(const entelea_wallclock_t *wall);
entelea_result_t
entelea_d12_bind_lock(const entelea_lock_t *lock);
entelea_result_t
entelea_d12_bind_store(const entelea_d12_store_t *store);

/** Platform boot report: whether this reset is an abnormal class (WDT or
 * panic), which decides the cooldown window. The platform boot guard calls
 * this; keeping counts across resets is the platform's job (reset_guard). */
void
entelea_d12_boot_note(bool abnormal_reset);

/** Abnormal-reset storm threshold: 3 or more consecutive abnormal resets cause
 * SAFE-HALT on the current boot. Counting from POR, this is the "4th boot"
 * (D12 v1 section 9). */
#define ENTELEA_D12_RESET_STORM_MAX 3U

/** Cross-reset storm decision (pure policy; I/O belongs to the platform).
 * abnormal: whether this reset is an abnormal class. history_ok: the retained
 * count is readable and valid. *count: the retained value on input, the
 * value to write back on output. Returns true for SAFE-HALT. An abnormal
 * reset with unknown history fails closed. A pure POR clears the count. The
 * count saturates rather than wrapping. SAFE-HALT is also required when count
 * is NULL. If the platform reset cause is unknown, or the retention device is
 * unavailable or a read or write fails, SAFE-HALT is required regardless of
 * reset type. */
bool
entelea_d12_reset_guard(bool abnormal, bool history_ok, uint32_t *count);

/** Pre-call decision for a model call (cooldown, frequency, daily quota, and
 * storm; the dependency side belongs to model_ask). OK = allowed; E_BUSY =
 * cooldown, frequency, or daily quota exhausted; E_STATE = storm latch,
 * unbound port, unknown wall clock, failed persistent read or write, or a
 * corrupted record. */
entelea_result_t
entelea_d12_model_allow(void);

/** Reports a model call result. Success clears the storm count; E_BUSY
 * triggers cooldown; failure accumulates toward the storm. */
void
entelea_d12_model_note(entelea_result_t r);

/** Storm latch state and external release (command surface or on-site); with
 * the clock or lock unbound, reports latched. */
bool
entelea_d12_storm_latched(void);
void
entelea_d12_storm_clear(void);

/** Pre-execution decision for wasm. Refused with E_BUSY within the cooldown
 * window; the storm does not affect wasm. */
entelea_result_t
entelea_d12_wasm_allow(void);

/** Observation surface: calls since start / calls refused by the gate. */
uint32_t
entelea_d12_model_calls(void);
uint32_t
entelea_d12_model_blocked(void);

#ifdef ENTELEA_TEST
void
entelea_d12_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_D12_H */
