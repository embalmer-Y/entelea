/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_WDT_H
#define ENTELEA_WDT_H

/* R02 soft watchdog framework contract: see
 * framework/runtime/supervision/contract.md.
 * Overdue -> ENTELEA_EVT_WDT_WARN -> entelea_safety_system_fail. */

#include <stdbool.h>
#include <stdint.h>
#include <entelea/result.h>
#include <entelea/supervision.h>
#include <entelea/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R02 source set: only SYWORK (liveness of the patrol itself). Legacy NET and
 * APPMGR sources are added with their subsystem batches; their numbers are
 * appended then. Existing values are not changed (DEC-48 registry semantics
 * are unchanged). */
typedef enum {
	ENTELEA_WDT_SYWORK,
	ENTELEA_WDT_COUNT,
} entelea_wdt_src_t;

/** Binds the instance clock. Calling register or start while unbound returns
 * ENTELEA_E_UNINIT and does not fabricate a default time base. Binding NULL
 * restores the unbound state.
 * feed may be called from an ISR; in that case the read_ms of the bound clock
 * must be ISR-safe (the platform uptime adapter satisfies this; see the
 * platform/zephyr contract). */
entelea_result_t
entelea_wdt_bind_clock(const entelea_clock_t *clock);

/** [thread] Registers a feed source during init. The period_ms semantics come
 * from DEC-22/27. Rejected after started. */
entelea_result_t
entelea_wdt_register(entelea_wdt_src_t src, uint32_t period_ms);

/** [any] Atomically updates last_feed. The first feed activates the source;
 * inactive sources are not part of the overdue check. */
void
entelea_wdt_feed(entelea_wdt_src_t src);

/** [any] Declarative blocking grace window (absolute deadline = now + budget;
 * clamped to 60s; cleared by feed; does not activate inactive sources).
 * Overdue is not judged inside the window. Once the window is exceeded,
 * overdue judgment resumes (fail-safe is kept). */
void
entelea_wdt_grace(entelea_wdt_src_t src, uint32_t budget_ms);

/** [thread] Deactivates a feed source. Not feeding after a stop is expected,
 * so it is not judged overdue; the next feed reactivates the source. */
void
entelea_wdt_deactivate(entelea_wdt_src_t src);

/** [thread] Starts patrolling. The sysworkq periodic item uses period =
 * min(periods)/2; with no registered source, patrols at the hard cap.
 * With CONFIG_TASK_WDT, attaches the L3 channel: callback=NULL means expiry
 * calls sys_reboot, the fallback for a hung soft patrol. Failures are returned
 * explicitly and never reported as success. */
entelea_result_t
entelea_wdt_start(void);

/** [any] Patrol period = min(periods)/2 (for observation and tests). */
uint32_t
entelea_wdt_patrol_period_ms(void);

/** [any] Global halt (called by system_fail). After it is set, new patrols
 * do not feed, reschedule, or judge overdue. Feed calls already in flight may
 * still complete. wdt_start is rejected. One-shot, with no release path. */
void
entelea_wdt_halt(void);

/** [any] Cumulative clock read failures and backward steps. Observed while the
 * monotonic guard reuses the last valid value. */
uint32_t
entelea_wdt_clock_faults(void);

/* Clock-blindness policy (A1, decided by the owner): after
 * ENTELEA_WDT_BLIND_ROUNDS consecutive patrol rounds in which time is unknown,
 * system_fail is raised (payload 0xFF is not a source number and marks clock
 * blindness). */
#define ENTELEA_WDT_BLIND_ROUNDS 3
#define ENTELEA_FAIL_WDT_CLOCK_BLIND ENTELEA_FAIL_WDT(0xFFu)

/* Value provenance (R02 compile-time constants; adjusted with the reference
 * board after the D12 board-level freeze): SYWORK period 1000ms,
 * task_wdt channel 5000ms, hard WDT cap 10000ms (DEC-48/22/27), and grace
 * window clamp 60000ms (DEC-64 #1). */
#define ENTELEA_WDT_SYWORK_PERIOD_MS 1000
#define ENTELEA_WDT_CHANNEL_MS 5000
#define ENTELEA_WDT_HARD_CAP_MS 10000
#define ENTELEA_WDT_GRACE_CAP_MS 60000

/* ENTELEA_TEST builds only: single-patrol check (returns the first overdue
 * source, or ENTELEA_WDT_COUNT if none) and grace-window liveness query. */
#ifdef ENTELEA_TEST
/** Test isolation: clears the source table, start state, halted state, and
 * clock guard. */
void
entelea_wdt_test_reset(void);
entelea_wdt_src_t
entelea_wdt_patrol_once(void);
bool
entelea_wdt_grace_active(entelea_wdt_src_t src);
void
entelea_wdt_test_after_elapsed_sample(void (*hook)(void));
/** Deterministically drives one patrol body (for halted-semantics tests).
 * Returns whether it actually ran. */
bool
entelea_wdt_test_patrol_tick(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_WDT_H */
