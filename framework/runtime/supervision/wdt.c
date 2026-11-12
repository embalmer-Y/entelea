/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Soft watchdog framework (migrated from the old core/wdt.c; R02 contract:
 * framework/runtime/supervision/contract.md).
 * Contract 4: each subsystem feeds its own watchdog, so a hang can be
 * located; overdue -> WDT_WARN -> system_fail.
 * Defense-layer placement (DEC-48): L1 is this soft watchdog (static
 * pre-registration plus dynamic activation: the boot step runs the start-up
 * patrol, and each subsystem's first feed activates its own source); L3 is the
 * Zephyr task_wdt channel (CONFIG_TASK_WDT; callback=NULL -> on expiry
 * sys_reboot, the fallback if the soft patrol itself hangs; the hardware
 * fallback goes through CONFIG_TASK_WDT_HW_FALLBACK plus the board-level
 * chosen zephyr,watchdog).
 * Changes from the old implementation: the time source is now bound to an
 * instance clock (feed ISR safety is described in the contract); the source
 * set in R02 is SYWORK only (NET/APPMGR join with their subsystem batches);
 * the noinit trace and last-reset readout belong to R03.
 */
#include <errno.h>
#include <string.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#ifdef CONFIG_TASK_WDT
#include <zephyr/device.h>
#include <zephyr/task_wdt/task_wdt.h>
#endif

struct wdt_src {
	uint32_t period_ms;
	atomic_t last_feed32; /* Clock low 32 bits; unsigned diff, wrap-safe */
	atomic_t grace_until32; /* Grace deadline (0 = none; feed clears) */
	bool registered;
	atomic_t active; /* Set by first feed; inactive sources are skipped */
};

static struct wdt_src srcs[ENTELEA_WDT_COUNT];
static bool started;
static const entelea_clock_t *wdt_clock;
static atomic_t halted = ATOMIC_INIT(0); /* Global halt (set by system_fail) */
static uint64_t clock_last_good;
static uint32_t clock_fault_count;
static uint8_t blind_rounds; /* Consecutive time-unknown rounds (A1 policy) */

#ifdef ENTELEA_TEST
static void (*after_elapsed_sample)(void);

void
entelea_wdt_test_after_elapsed_sample(void (*hook)(void))
{
	after_elapsed_sample = hook;
}
#endif

#ifdef CONFIG_TASK_WDT
static int twdt_ch = -1;
static bool twdt_initialized;
#endif

entelea_result_t
entelea_wdt_bind_clock(const entelea_clock_t *clock)
{
	if (clock == NULL || clock->read_ms == NULL || clock->clock_id == 0) {
		/* NULL binding restores unbound state (test isolation). */
		if (clock == NULL) {
			wdt_clock = NULL;
			return ENTELEA_OK;
		}
		return ENTELEA_E_PARAM;
	}
	wdt_clock = clock;
	return ENTELEA_OK;
}

/* Raw read of the bound instance clock (feed must be callable from ISR, so
 * this bypasses the non-atomic regression bookkeeping of clock_now; ISR
 * safety is a requirement declared with the binding, see wdt.h).
 * Monotonic guard: on read failure or regression the last valid value is
 * kept and counted. Faking 0 would make elapsed jump and cause a false
 * overdue (a fail misfire).
 * Concurrency (second-round review defect 5): last_good and the failure count
 * are 64/32-bit read-modify-write; threads (patrol/tests) and the ISR (feed)
 * access them concurrently, and u64 accesses can tear on 32-bit targets.
 * All compare, update and count operations go inside an irq_lock micro
 * critical section (nestable in ISR).
 * Returns false = time unknown for this call (guard intervened); *out holds
 * the last valid value. */
static bool
now32_checked(uint32_t *out)
{
	uint64_t ms = 0;
	unsigned int key = irq_lock();

	/* Read, compare and update all run under the lock (reads must be
	 * short and non-blocking, per the platform contract). Thread and ISR
	 * sampling is fully serialized, so an "earlier sample, later
	 * registration" interleaving can no longer happen. Inside the lock
	 * only a real regression (the clock itself went backward) remains:
	 * count a fault and conservatively keep the last valid value
	 * (third-round review finding 3: a real regression must be told
	 * apart from a normal interleaving, and must not cause a false
	 * halt). */
	bool ok = wdt_clock != NULL &&
		  wdt_clock->read_ms(wdt_clock->user, &ms) == ENTELEA_OK;
	bool known = ok;

	if (!ok || ms < clock_last_good) {
		clock_fault_count++;
		ms = clock_last_good;
		known = false;
	} else {
		clock_last_good = ms;
	}
	irq_unlock(key);
	*out = (uint32_t) ms;
	/* Time is known only if the read succeeded **and** no regression
	 * occurred: a real regression (reading a stale value) is reported as
	 * unknown, like a read failure. The contract says a real regression
	 * still counts toward A1 blindness rounds (plan-conformance review P2:
	 * before the fix, returning true on regression reset blind_rounds to
	 * zero, so A1 silently failed with a persistently regressing clock
	 * backend). */
	return known;
}

static uint32_t
now32(void)
{
	uint32_t v;

	(void) now32_checked(&v);
	return v;
}

uint32_t
entelea_wdt_clock_faults(void)
{
	unsigned int key = irq_lock();
	uint32_t count = clock_fault_count;

	irq_unlock(key);
	return count;
}

void
entelea_wdt_halt(void)
{
	/* Global halt (defect 4): once set, the patrol neither feeds,
	 * reschedules, nor checks for overdue. Any thread triggering
	 * system_fail gets the same halt semantics and does not depend on the
	 * calling thread suspending itself. After feeding stops, task_wdt
	 * expiry -> sys_reboot is the fallback. */
	atomic_set(&halted, 1);
}

entelea_result_t
entelea_wdt_register(entelea_wdt_src_t src, uint32_t period_ms)
{
	if ((int) src < 0 || src >= ENTELEA_WDT_COUNT || period_ms == 0 ||
	    started) {
		return ENTELEA_E_PARAM;
	}
	if (wdt_clock == NULL) {
		return ENTELEA_E_UNINIT; /* No fake default time base */
	}
	if (srcs[src].registered) {
		return ENTELEA_E_STATE;
	}
	srcs[src].registered = true;
	srcs[src].period_ms = period_ms;
	atomic_set(&srcs[src].last_feed32, (atomic_val_t) now32());
	return ENTELEA_OK;
}

void
entelea_wdt_feed(entelea_wdt_src_t src)
{
	/* Halt entry guard (third-round review finding 6): once halt is set,
	 * feeds from any call path are suppressed at entry, including a
	 * patrol halted between the halt check and the feed. Granularity is
	 * per call entry; at most one call already in flight may still get
	 * through. */
	if (atomic_get(&halted) != 0) {
		return;
	}
	if ((int) src >= 0 && src < ENTELEA_WDT_COUNT) {
		unsigned int key = irq_lock();

		atomic_set(&srcs[src].last_feed32, (atomic_val_t) now32());
		/* A normal feed clears the grace window. */
		atomic_set(&srcs[src].grace_until32, 0);
		atomic_set(&srcs[src].active, 1); /* First feed = activate */
		irq_unlock(key);
	}
}

void
entelea_wdt_grace(entelea_wdt_src_t src, uint32_t budget_ms)
{
	if ((int) src < 0 || src >= ENTELEA_WDT_COUNT || budget_ms == 0) {
		return;
	}
	struct wdt_src *s = &srcs[src];

	/* Grace never activates a source (activation stays feed-only). */
	if (!s->registered || atomic_get(&s->active) == 0) {
		return;
	}
	if (budget_ms > ENTELEA_WDT_GRACE_CAP_MS) {
		budget_ms = ENTELEA_WDT_GRACE_CAP_MS;
	}
	/* Absolute deadline = now + budget; u32 wraparound is absorbed by the
	 * signed-difference comparison. */
	atomic_set(&s->grace_until32, (atomic_val_t) (now32() + budget_ms));
}

void
entelea_wdt_deactivate(entelea_wdt_src_t src)
{
	/* Deactivation mirror: the source stops with its subsystem, so no
	 * feeds after it stops is expected and not overdue; the next feed
	 * reactivates it. */
	if ((int) src >= 0 && src < ENTELEA_WDT_COUNT) {
		atomic_set(&srcs[src].active, 0);
	}
}

/* Patrol: returns the first overdue source, or ENTELEA_WDT_COUNT if none.
 * Clock-blindness policy (A1, owner-approved 2026-11-08): a single read
 * failure means the time is unknown for this round; it neither marks a
 * source overdue nor assumes liveness. After ENTELEA_WDT_BLIND_ROUNDS or
 * more consecutive unknown rounds, supervision cannot keep time -> system_fail
 * (fail reason payload 0xFF = clock blindness, not a source number). */
static entelea_wdt_src_t
patrol_once_impl(void)
{
	uint32_t now;
	uint32_t last_feed[ENTELEA_WDT_COUNT];

	for (int s = 0; s < ENTELEA_WDT_COUNT; s++) {
		last_feed[s] = (uint32_t) atomic_get(&srcs[s].last_feed32);
	}

	if (!now32_checked(&now)) {
		if (++blind_rounds >= ENTELEA_WDT_BLIND_ROUNDS) {
			entelea_safety_system_fail(
				ENTELEA_FAIL_WDT_CLOCK_BLIND);
			return ENTELEA_WDT_COUNT;
		}
		return ENTELEA_WDT_COUNT; /* Single transient: not overdue */
	}
	blind_rounds = 0;

#ifdef ENTELEA_TEST
	if (after_elapsed_sample != NULL) {
		after_elapsed_sample();
	}
#endif

	for (int s = 0; s < ENTELEA_WDT_COUNT; s++) {
		if (!srcs[s].registered || atomic_get(&srcs[s].active) == 0) {
			continue;
		}
		if (last_feed[s] !=
		    (uint32_t) atomic_get(&srcs[s].last_feed32)) {
			/* Concurrent feed changed the snapshot: re-check next
			 * round to avoid underflow from the new timestamp. */
			continue;
		}
		if (now - last_feed[s] > srcs[s].period_ms) {
			/* Within the grace window: no overdue (declared, legal
			 * blocking). After the window: overdue again (budget
			 * expired = real hang; fail-safe stays). */
			uint32_t gu =
				(uint32_t) atomic_get(&srcs[s].grace_until32);

			if (gu != 0 && (int32_t) (now - gu) < 0) {
				continue;
			}
			/* WDT_WARN first (carries src and last feed time),
			 * then system_fail. */
			entelea_wdt_warn_evt_t payload = {
				.src = (int32_t) s,
				.last_feed = last_feed[s],
			};
			const entelea_event_t evt = {
				.id = ENTELEA_EVT_WDT_WARN,
				.data = &payload,
				.len = sizeof(payload),
			};

			entelea_evt_publish(&evt);
			return (entelea_wdt_src_t) s;
		}
	}
	return ENTELEA_WDT_COUNT;
}

static void
patrol_work_cb(struct k_work *work)
{
	struct k_work_delayable *d = k_work_delayable_from_work(work);

	if (atomic_get(&halted) != 0) {
		return; /* Halted: no feed or reschedule (feed self-guards) */
	}
	entelea_wdt_feed(
		ENTELEA_WDT_SYWORK); /* Patrol run = sysworkq liveness proof */
#ifdef CONFIG_TASK_WDT
	if (twdt_ch >= 0 && atomic_get(&halted) == 0) {
		task_wdt_feed(
			twdt_ch); /* L3: patrol hang -> expiry -> sys_reboot */
	}
#endif
	/* Deferred estop publish (reuses patrol cadence). */
	entelea_safety_estop_deferred_publish();
	entelea_wdt_src_t overdue = patrol_once_impl();

	if (overdue != ENTELEA_WDT_COUNT) {
		entelea_safety_system_fail(
			ENTELEA_FAIL_WDT((uint32_t) overdue));
		/* system_fail sets halted (before the halt itself): the patrol
		 * no longer feeds or reschedules. With no feed after halt,
		 * the task_wdt channel expires and sys_reboot closes the
		 * reset loop. */
		return;
	}
	uint32_t delay = entelea_wdt_patrol_period_ms();

	k_work_reschedule_for_queue(&k_sys_work_q, d, K_MSEC(delay));
}

static struct k_work_delayable patrol_work;

uint32_t
entelea_wdt_patrol_period_ms(void)
{
	uint32_t min_p = UINT32_MAX;

	for (int s = 0; s < ENTELEA_WDT_COUNT; s++) {
		if (srcs[s].registered && srcs[s].period_ms < min_p) {
			min_p = srcs[s].period_ms;
		}
	}
	if (min_p == UINT32_MAX) {
		min_p = ENTELEA_WDT_HARD_CAP_MS; /* No sources: hard WDT cap */
	}
	return min_p / 2 < 1 ? 1 : min_p / 2; /* Period: min(periods)/2 */
}

entelea_result_t
entelea_wdt_start(void)
{
	if (started || atomic_get(&halted) != 0) {
		return ENTELEA_E_STATE;
	}
	if (wdt_clock == NULL) {
		return ENTELEA_E_UNINIT; /* No fake default time base */
	}
	/* Static pre-registration (period constants: DEC-48; the registry is
	 * immutable from here on, so there is no concurrency surface).
	 * Activation = each subsystem's first feed. */
	srcs[ENTELEA_WDT_SYWORK].registered = true;
	srcs[ENTELEA_WDT_SYWORK].period_ms = ENTELEA_WDT_SYWORK_PERIOD_MS;
	atomic_set(&srcs[ENTELEA_WDT_SYWORK].last_feed32,
		   (atomic_val_t) now32());

#ifdef CONFIG_TASK_WDT
	const struct device *hw = NULL;
	int ret;

#if defined(ENTELEA_WDT_REQUIRE_HW) && !DT_HAS_CHOSEN(zephyr_watchdog)
	return ENTELEA_E_IO; /* Required HW watchdog not chosen: no fallback */
#endif
#if defined(CONFIG_TASK_WDT_HW_FALLBACK) && DT_HAS_CHOSEN(zephyr_watchdog)
	hw = DEVICE_DT_GET(DT_CHOSEN(zephyr_watchdog));
	if (!device_is_ready(hw)) {
		return ENTELEA_E_IO;
	}
#endif
	if (!twdt_initialized) {
		ret = task_wdt_init(hw);
		if (ret != 0) {
			return ENTELEA_E_IO;
		}
		twdt_initialized = true;
	}
	/* callback=NULL: expiry -> sys_reboot(COLD) (task_wdt semantics). */
	twdt_ch = task_wdt_add(ENTELEA_WDT_CHANNEL_MS, NULL, NULL);
	if (twdt_ch < 0) {
		return twdt_ch == -ENOMEM ? ENTELEA_E_NOMEM : ENTELEA_E_IO;
	}
#endif

	started = true;
	k_work_init_delayable(&patrol_work, patrol_work_cb);
	k_work_reschedule_for_queue(&k_sys_work_q, &patrol_work,
				    K_MSEC(entelea_wdt_patrol_period_ms()));
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void
entelea_wdt_test_reset(void)
{
	/* Test isolation: first synchronously cancel the patrol item that may
	 * still be scheduled (re-initializing a scheduled delayable is
	 * undefined behavior), then clear the source table, started flag,
	 * halted, clock guard and blind rounds. */
	struct k_work_sync sync;

	(void) k_work_cancel_delayable_sync(&patrol_work, &sync);
	memset(srcs, 0, sizeof(srcs));
	started = false;
	atomic_set(&halted, 0);
	clock_last_good = 0;
	clock_fault_count = 0;
	blind_rounds = 0;
	after_elapsed_sample = NULL;
}

entelea_wdt_src_t
entelea_wdt_patrol_once(void)
{
	return patrol_once_impl();
}

bool
entelea_wdt_test_patrol_tick(void)
{
	/* Deterministic patrol-body driver (for halted tests): returns whether
	 * it actually ran. */
	if (atomic_get(&halted) != 0) {
		return false;
	}
	patrol_work_cb(&patrol_work.work);
	return true;
}

bool
entelea_wdt_grace_active(entelea_wdt_src_t src)
{
	if ((int) src < 0 || src >= ENTELEA_WDT_COUNT) {
		return false;
	}
	uint32_t gu = (uint32_t) atomic_get(&srcs[src].grace_until32);

	return gu != 0 && (int32_t) (now32() - gu) < 0;
}
#endif
