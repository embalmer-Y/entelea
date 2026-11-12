/* SPDX-License-Identifier: Apache-2.0 */
/* D12 runtime gate implementation (contract: see the include/entelea/d12.h
 * header comment; thresholds: the owner-approved v1 conservative plan).
 * Entry is serialized by a mutex port; an unbound gate refuses.
 * Monotonic time is clamped (on regression the last valid value is reused:
 * windows and cooldowns may only extend, never shrink). The cooldown is
 * suspended while the clock is blind and starts at the first good read.
 * The daily quota follows UTC day boundaries and is saved through a durable
 * port: before admitting, confirm the day's reserved ceiling has headroom;
 * when headroom runs out, write a new reservation before admitting. */
#include <string.h>
#include <entelea/time.h>
#include <entelea/d12.h>
#include <entelea/result.h>

#define D12_COOLDOWN_MS 60000U /* E_BUSY cooldown (429 surface) */
#define D12_BOOT_COOLDOWN_MS 60000U /* Service cooldown after abnormal reset */
#define D12_RATE_WINDOW_MS 60000U
#define D12_RATE_MAX_IN_WINDOW 6U
#define D12_SECS_PER_DAY 86400U
#define D12_REC_VERSION 1U
#define D12_STORM_FAILS 3U /* Consecutive-failure latch threshold */

static const entelea_clock_t *d12_clock;
static const entelea_wallclock_t *d12_wall;
static const entelea_lock_t *d12_lock;
static const entelea_d12_store_t *d12_store;
static const uint8_t d12_rec_magic[4] = { 'D', '1', '2', 'G' };
static uint64_t last_t; /* Monotonic clamp baseline */
static uint64_t cooldown_start;
static uint64_t boot_cooldown_start;
static bool cooldown_active;
static bool boot_cooldown_active;
static bool cooldown_pending; /* Clock blind at report; await first good read */
static bool boot_cooldown_pending;
static uint64_t window[D12_RATE_MAX_IN_WINDOW]; /* Sliding window (ring) */
static uint8_t window_next; /* Next write slot (the oldest when full) */
static uint8_t window_used;
static uint32_t calls_total;
static uint32_t blocked_total;
static uint8_t fail_streak;
static bool day_loaded; /* Persisted record read; effective day set */
static uint32_t day_utc; /* UTC day in effect (monotonic, forward only) */
static uint16_t day_reserved; /* Persisted admission ceiling for this day */
static uint16_t day_count; /* Admissions granted this day */
static bool storm_latched; /* Released only by storm_clear */

static bool
now_ms(uint64_t *out)
{
	uint64_t t;

	if (d12_clock == NULL ||
	    d12_clock->read_ms(d12_clock->user, &t) != ENTELEA_OK) {
		return false;
	}
	if (t < last_t) {
		t = last_t; /* Regression: no underflow to widen windows */
	}
	last_t = t;
	if (boot_cooldown_pending) {
		boot_cooldown_start = t;
		boot_cooldown_active = true;
		boot_cooldown_pending = false;
	}
	if (cooldown_pending) {
		cooldown_start = t;
		cooldown_active = true;
		cooldown_pending = false;
	}
	*out = t;
	return true;
}

bool
entelea_d12_reset_guard(bool abnormal, bool history_ok, uint32_t *count)
{
	if (count == NULL) {
		return true;
	}
	if (!abnormal) {
		*count = 0; /* Pure POR: exit storm window */
		return false;
	}
	if (!history_ok) {
		/* Abnormal reset with unknown cross-reset history (retention
		 * unavailable or magic corrupt, e.g. power-loss reset losing
		 * LP RAM): cannot rule out a storm, so latch closed. */
		*count = ENTELEA_D12_RESET_STORM_MAX;
		return true;
	}
	if (*count >= ENTELEA_D12_RESET_STORM_MAX) {
		*count = ENTELEA_D12_RESET_STORM_MAX;
		return true;
	}
	(*count)++;
	return *count >= ENTELEA_D12_RESET_STORM_MAX;
}

entelea_result_t
entelea_d12_bind_clock(const entelea_clock_t *c)
{
	if (c == NULL || c->read_ms == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_clock = c; /* Storm latch is not released by rebinding */
	return ENTELEA_OK;
}

/* Bound at startup (read-only at run time); an unbound port makes
 * dependent checks refuse. */
entelea_result_t
entelea_d12_bind_wallclock(const entelea_wallclock_t *wall)
{
	if (wall == NULL || wall->read_utc_s == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_wall = wall;
	return ENTELEA_OK;
}

entelea_result_t
entelea_d12_bind_lock(const entelea_lock_t *lock)
{
	if (lock == NULL || lock->lock == NULL || lock->unlock == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_lock = lock;
	return ENTELEA_OK;
}

entelea_result_t
entelea_d12_bind_store(const entelea_d12_store_t *store)
{
	if (store == NULL || store->load == NULL || store->save == NULL) {
		return ENTELEA_E_PARAM;
	}
	d12_store = store;
	return ENTELEA_OK;
}

/* Enter the critical section. Returns non-OK if the lock is unbound or the
 * lock port refuses (ISR); the caller must not leave. */
static entelea_result_t
d12_enter(void)
{
	if (d12_lock == NULL) {
		return ENTELEA_E_STATE;
	}
	return d12_lock->lock(d12_lock->user);
}

static void
d12_leave(void)
{
	d12_lock->unlock(d12_lock->user);
}

void
entelea_d12_boot_note(bool abnormal_reset)
{
	uint64_t t;

	if (!abnormal_reset) {
		return;
	}
	if (d12_lock != NULL && d12_enter() == ENTELEA_OK) {
		boot_cooldown_pending = true;
		(void) now_ms(&t);
		d12_leave();
		return;
	}
	/* Lock not bound yet (single-threaded startup): pend the cooldown,
	 * start it at the first good read. */
	boot_cooldown_pending = true;
}

static bool
in_boot_cooldown(uint64_t t)
{
	return boot_cooldown_active &&
	       t - boot_cooldown_start < D12_BOOT_COOLDOWN_MS;
}

static bool
in_cooldown(uint64_t t)
{
	return (cooldown_active && t - cooldown_start < D12_COOLDOWN_MS) ||
	       in_boot_cooldown(t);
}

static bool
rate_ok(uint64_t t)
{
	/* Sliding window: admit while not full; when full, check whether the
	 * oldest slot has left the window (t is monotonic, no underflow). */
	return window_used < D12_RATE_MAX_IN_WINDOW ||
	       t - window[window_next] >= D12_RATE_WINDOW_MS;
}

/* Persistent record (12 bytes, little-endian, reserved KV key): magic
 * "D12G", version, flags=0, reserved (persisted ceiling for the day) u16,
 * utc_day (UTC day number) u32. */
static void
rec_encode(uint8_t rec[ENTELEA_D12_REC_LEN], uint32_t day, uint16_t reserved)
{
	memcpy(rec, d12_rec_magic, sizeof(d12_rec_magic));
	rec[4] = D12_REC_VERSION;
	rec[5] = 0U;
	rec[6] = (uint8_t) (reserved & 0xFFU);
	rec[7] = (uint8_t) (reserved >> 8);
	rec[8] = (uint8_t) (day & 0xFFU);
	rec[9] = (uint8_t) ((day >> 8) & 0xFFU);
	rec[10] = (uint8_t) ((day >> 16) & 0xFFU);
	rec[11] = (uint8_t) ((day >> 24) & 0xFFU);
}

static bool
rec_decode(const uint8_t rec[ENTELEA_D12_REC_LEN], uint32_t *day,
	   uint16_t *reserved)
{
	uint16_t r;

	if (memcmp(rec, d12_rec_magic, sizeof(d12_rec_magic)) != 0 ||
	    rec[4] != D12_REC_VERSION || rec[5] != 0U) {
		return false;
	}
	r = (uint16_t) (rec[6] | (rec[7] << 8));
	if (r > ENTELEA_D12_DAILY_MAX) {
		return false;
	}
	*reserved = r;
	*day = (uint32_t) rec[8] | ((uint32_t) rec[9] << 8) |
	       ((uint32_t) rec[10] << 16) | ((uint32_t) rec[11] << 24);
	return true;
}

/* Read the persistent record on first use. No record = start the day from
 * zero; read failure or a corrupt record = refuse, never admit on empty
 * state. After a restart, count takes reserved: this can only be
 * conservative, never exceed the ceiling. */
static entelea_result_t
daily_load(uint32_t today)
{
	uint8_t rec[ENTELEA_D12_REC_LEN];
	uint32_t stored_day = 0;
	uint16_t stored_reserved = 0;
	entelea_result_t r;

	if (d12_store == NULL) {
		return ENTELEA_E_STATE;
	}
	r = d12_store->load(d12_store->user, rec);
	if (r == ENTELEA_E_NOTFOUND) {
		day_utc = today;
		day_reserved = 0;
		day_count = 0;
		day_loaded = true;
		return ENTELEA_OK;
	}
	if (r != ENTELEA_OK ||
	    !rec_decode(rec, &stored_day, &stored_reserved)) {
		return ENTELEA_E_STATE;
	}
	if (stored_day < today) {
		day_utc = today;
		day_reserved = 0;
		day_count = 0;
	} else {
		/* Same day, or the wall clock went back (stored day is later):
		 * keep the later day's ceiling; do not loosen it. */
		day_utc = stored_day;
		day_reserved = stored_reserved;
		day_count = stored_reserved;
	}
	day_loaded = true;
	return ENTELEA_OK;
}

/* Daily quota check: when headroom runs out, reserve in steps, writing
 * before admitting; a failed write refuses. */
static entelea_result_t
daily_admit(void)
{
	uint64_t secs = 0;
	uint32_t today;
	uint16_t next;
	uint8_t rec[ENTELEA_D12_REC_LEN];
	entelea_result_t r;

	if (d12_wall == NULL || d12_store == NULL ||
	    d12_wall->read_utc_s(d12_wall->user, &secs) != ENTELEA_OK ||
	    secs / D12_SECS_PER_DAY > UINT32_MAX) {
		return ENTELEA_E_STATE; /* Clock unknown or day out of range */
	}
	today = (uint32_t) (secs / D12_SECS_PER_DAY);
	if (!day_loaded) {
		r = daily_load(today);
		if (r != ENTELEA_OK) {
			return r;
		}
	}
	if (today > day_utc) {
		/* New day: start from zero. On regression (today < day_utc),
		 * keep the later day. */
		day_utc = today;
		day_reserved = 0;
		day_count = 0;
	}
	if (day_count < day_reserved) {
		day_count++;
		return ENTELEA_OK;
	}
	if (day_reserved >= ENTELEA_D12_DAILY_MAX) {
		return ENTELEA_E_BUSY; /* Daily quota exhausted */
	}
	next = (uint16_t) (day_reserved + ENTELEA_D12_RESERVE_STEP);
	if (next > ENTELEA_D12_DAILY_MAX) {
		next = (uint16_t) ENTELEA_D12_DAILY_MAX;
	}
	rec_encode(rec, day_utc, next);
	if (d12_store->save(d12_store->user, rec) != ENTELEA_OK) {
		return ENTELEA_E_STATE; /* Write failed: refuse, state kept */
	}
	day_reserved = next;
	day_count++;
	return ENTELEA_OK;
}

static entelea_result_t
model_allow_locked(void)
{
	uint64_t t;
	entelea_result_t r;

	if (!now_ms(&t)) {
		return ENTELEA_E_STATE; /* No monotonic clock: fail closed */
	}
	if (storm_latched) {
		blocked_total++;
		return ENTELEA_E_STATE;
	}
	if (in_cooldown(t) || !rate_ok(t)) {
		blocked_total++;
		return ENTELEA_E_BUSY;
	}
	r = daily_admit();
	if (r != ENTELEA_OK) {
		blocked_total++;
		return r;
	}
	window[window_next] = t;
	window_next = (uint8_t) ((window_next + 1U) % D12_RATE_MAX_IN_WINDOW);
	if (window_used < D12_RATE_MAX_IN_WINDOW) {
		window_used++;
	}
	calls_total++;
	return ENTELEA_OK;
}

entelea_result_t
entelea_d12_model_allow(void)
{
	entelea_result_t r = d12_enter();

	if (r != ENTELEA_OK) {
		return r;
	}
	r = model_allow_locked();
	d12_leave();
	return r;
}

void
entelea_d12_model_note(entelea_result_t r)
{
	uint64_t t;

	if (d12_enter() != ENTELEA_OK) {
		return; /* Lock not bound: refused earlier, no state */
	}
	if (r == ENTELEA_OK) {
		fail_streak = 0;
	} else {
		if (r == ENTELEA_E_BUSY) {
			cooldown_pending = true;
			(void) now_ms(&t);
		}
		if (++fail_streak >= D12_STORM_FAILS) {
			storm_latched = true; /* Cleared externally only */
			fail_streak = 0;
		}
	}
	d12_leave();
}

bool
entelea_d12_storm_latched(void)
{
	bool latched;

	if (d12_clock == NULL || d12_lock == NULL) {
		return true; /* Clock or lock unbound: conservatively latched */
	}
	if (d12_enter() != ENTELEA_OK) {
		return true;
	}
	latched = storm_latched;
	d12_leave();
	return latched;
}

void
entelea_d12_storm_clear(void)
{
	if (d12_enter() != ENTELEA_OK) {
		return;
	}
	storm_latched = false;
	fail_streak = 0;
	d12_leave();
}

entelea_result_t
entelea_d12_wasm_allow(void)
{
	uint64_t t;
	entelea_result_t r = d12_enter();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (!now_ms(&t)) {
		r = ENTELEA_E_STATE;
	} else if (in_boot_cooldown(t)) {
		r = ENTELEA_E_BUSY;
	} else {
		r = ENTELEA_OK;
	}
	d12_leave();
	return r;
}

uint32_t
entelea_d12_model_calls(void)
{
	uint32_t n = 0;

	if (d12_enter() == ENTELEA_OK) {
		n = calls_total;
		d12_leave();
	}
	return n;
}

uint32_t
entelea_d12_model_blocked(void)
{
	uint32_t n = 0;

	if (d12_enter() == ENTELEA_OK) {
		n = blocked_total;
		d12_leave();
	}
	return n;
}

#ifdef ENTELEA_TEST
void
entelea_d12_test_reset(void)
{
	d12_clock = NULL;
	d12_wall = NULL;
	d12_lock = NULL;
	d12_store = NULL;
	last_t = 0;
	cooldown_start = 0;
	boot_cooldown_start = 0;
	cooldown_active = false;
	boot_cooldown_active = false;
	cooldown_pending = false;
	boot_cooldown_pending = false;
	memset(window, 0, sizeof(window));
	window_next = 0;
	window_used = 0;
	calls_total = 0;
	blocked_total = 0;
	fail_streak = 0;
	storm_latched = false;
	day_loaded = false;
	day_utc = 0;
	day_reserved = 0;
	day_count = 0;
}
#endif
