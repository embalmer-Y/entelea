/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_SAFETY_H
#define ENTELEA_SAFETY_H

/* R02 non-bypassable output protection contract: see
 * framework/execution/safety/contract.md.
 * All product outputs must go through this module's single write path. The only
 * Zephyr output-driver call site in the repository is
 * framework/execution/safety/drivers.c (the source-policy protection gate).
 * Trimming relative to the legacy ts-safety (audit ring -> R03; control-source
 * arbitration -> R04; prov-based estop edge and trace persistence -> R03;
 * generic GPIO input backend -> later batches) is recorded in the contract and
 * the ledger. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/events.h>
#include <entelea/result.h>
#include <entelea/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Channel model ---- */

typedef enum {
	ENTELEA_CH_GPIO,
	ENTELEA_CH_PWM,
	ENTELEA_CH_POWER,
	ENTELEA_CH_KIND_COUNT,
} entelea_ch_kind_t;

typedef union {
	bool b; /* ENTELEA_CH_GPIO */
	uint32_t u; /* ENTELEA_CH_PWM */
	struct {
		bool en;
		uint32_t ma;
	} pwr; /* ENTELEA_CH_POWER (controlled power) */
} entelea_out_value_t;

typedef struct {
	const char *uid; /* stable logical name (frozen at registration) */
	entelea_ch_kind_t kind;
	entelea_out_value_t poweron; /* all three safe states; else refused */
	entelea_out_value_t linkloss;
	entelea_out_value_t fault;
	struct {
		uint32_t min, max; /* clamp range (GPIO ignores) */
		uint32_t slew_per_ms; /* max slew rate (0 = unlimited) */
		uint32_t current_limit_ma; /* supply current limit (POWER) */
	} limits;
} entelea_out_channel_t;

/* Three-safe-state state machine */
typedef enum {
	ENTELEA_ST_SAFE_POWERON,
	ENTELEA_ST_ACTIVE,
	ENTELEA_ST_SAFE_LINKLOSS,
	ENTELEA_ST_SAFE_FAULT,
	ENTELEA_CH_STATE_COUNT,
} entelea_ch_state_t;

/** [thread] At init and during peripheral registration. A missing safe state,
 * an out-of-range value, or an invalid or duplicate uid returns
 * ENTELEA_E_PARAM (registration fails outright; no partial registration is
 * kept). Capacity: ENTELEA_SAFETY_MAX_CHANNELS. */
entelea_result_t
entelea_safety_register_channel(const entelea_out_channel_t *ch);

/** Binds the instance clock (the source of slew dt and estop timestamps).
 * Entry points that return ENTELEA_E_UNINIT when unbound are marked in the
 * contract. Binding NULL restores the unbound state. */
entelea_result_t
entelea_safety_bind_clock(const entelea_clock_t *clock);

/* ---- Single write path (protection enforcement point) ---- */

/** [thread] The protection check that every output must pass (clamp -> slew
 * -> current limit -> final critical section). Outside ACTIVE state, or during
 * a fail-safe latch, returns ENTELEA_E_STATE. A power over-limit rejects the
 * whole request with ENTELEA_E_RANGE. When clamping or slew limiting splits
 * the change, returns the value actually applied and reports ENTELEA_E_RANGE.
 * An ISR caller gets ENTELEA_E_PERM. */
entelea_result_t
entelea_safety_commit(const char *uid, entelea_out_value_t v);

/** [thread] Reused by callers that already hold write_lock (the R04 atomic
 * budget-check-and-commit path). R02 has no external holder; this exists only
 * to keep the contract complete. */
struct entelea_safety_pend;
entelea_result_t
entelea_safety_commit_locked(const char *uid, entelea_out_value_t v,
			     struct entelea_safety_pend *pend);

/** [any] Reads back the shadow value (does not go through the driver). */
entelea_result_t
entelea_safety_readback(const char *uid, entelea_out_value_t *out);

/** [any] Current safety state of the channel (observation point). */
entelea_result_t
entelea_safety_channel_state(const char *uid, entelea_ch_state_t *out);

/** Canonical single-word encoding of entelea_out_value_t (b -> 0/1; pwr ->
 * en<<31 | ma&0x7FFFFFFF; otherwise .u). */
static inline uint32_t
entelea_value_encode(entelea_ch_kind_t k, entelea_out_value_t v)
{
	switch (k) {
	case ENTELEA_CH_GPIO:
		return v.b ? 1U : 0U;
	case ENTELEA_CH_POWER:
		return (v.pwr.en ? 1U << 31 : 0U) | (v.pwr.ma & 0x7FFFFFFFU);
	default:
		return v.u;
	}
}

/** Observation summary: counts per state and total registrations. Read without
 * a lock; the observation surface tolerates torn reads. */
typedef struct {
	uint16_t by_state[ENTELEA_CH_STATE_COUNT];
	uint16_t channels;
} entelea_safety_summary_t;

entelea_result_t
entelea_safety_summary(entelea_safety_summary_t *out);

/* ---- estop and fail-safe direct paths ---- */

/** [ISR] Called directly from the estop GPIO callback. It atomically sets
 * forced, then writes the fault value directly on each channel. The call graph
 * must not allocate, use queues or locks, or touch the protocol stack. */
void
entelea_safety_force_all_fault(void);

/** [thread] WDT, BOOT, or subsystem fault: all channels enter SAFE_FAULT, and
 * the controlled halt sequence runs. In ENTELEA_TEST builds, the state is set
 * and control returns, so tests can assert the halt path. Before halting, the
 * reason is recorded via entelea_safety_record_fail_reason (the storage
 * module's strong implementation writes it to noinit; builds without storage
 * use a weak stub, so the missing record is reported as such). */
void
entelea_safety_system_fail(uint32_t reason);

/** Hook that records the system_fail reset reason (weak stub in force.c; strong
 * implementation in the R03 storage module). */
void
entelea_safety_record_fail_reason(uint32_t reason);

/** [thread] Link-state transition (serialized by sysworkq). Hysteresis is
 * handled by the R06 consumer. If the write fails, the backend returns an
 * error, ENTELEA_E_IO is returned without updating the shadow or state. */
entelea_result_t
entelea_safety_set_link(bool up);

/* ---- Single-channel fault/recovery (detach mechanism) ---- */

/** [thread] Physically absent = single-channel fault (the global estop latch is
 * not set). */
entelea_result_t
entelea_safety_force_channel_fault(const char *uid);

/** [thread] Reattached = power-on semantics: returns to the poweron value.
 * Recovery is refused while the global latch is set. */
entelea_result_t
entelea_safety_channel_recover(const char *uid);

/** [thread] The only release path for the estop latch. Recovery is
 * conditional: if the link is up, go to ACTIVE; otherwise go to
 * SAFE_LINKLOSS. Output values are not written back; the fault safe value is
 * kept until the next explicit commit. */
entelea_result_t
entelea_safety_clear_fault(void);

/** [thread] Deferred estop publication: sysworkq periodically detects the
 * forced rising edge and publishes ENTELEA_EVT_ESTOP. No work item is
 * submitted from the ISR. */
void
entelea_safety_estop_deferred_publish(void);

/* ---- Boot step implementations ---- */

/** [thread] estop DT binding. Boards without the node (native_sim or tests)
 * make this a no-op that returns OK. Production boards must provide it; the
 * board-level assertion is frozen with the reference board. */
entelea_result_t
entelea_safety_estop_init(void);

/** [thread] Sets all channels to the initial SAFE_POWERON state (the declared
 * values are applied to the driver). */
entelea_result_t
entelea_safety_poweron_init(void);

/** Maps the estop trigger edge. 0 = rising (default), 1 = falling, 2 = both
 * edges; unknown values fail safe to rising. The prov-based configuration
 * surface belongs to R03; R02 uses the fixed default. */
int
entelea_safety_estop_edge_of(uint8_t flags);

/** estop direct trigger latch (set by the ISR, reset by clear_fault). This is
 * an observation surface, read atomically without a lock (r14 real-hardware
 * gate observation). */
bool
entelea_safety_estop_latched(void);

/** Time of the most recent estop trigger (bound clock, in ms; 0 if never
 * triggered). This is a factual record. clear_fault releases the latch but
 * does not clear the time, because EVT_ESTOP edge publication depends on it.
 * This observation surface is read atomically without a lock. */
uint32_t
entelea_safety_estop_latched_at(void);

/** Raw physical level of the estop pin (0/1; -1 if unbound or the read
 * fails), for wiring diagnosis and on-site observation (r14: tells apart
 * "wiring unchanged" from "level changed but no edge fired"). */
int
entelea_safety_estop_level_raw(void);

/* ---- Event payload (published by this module) ---- */

typedef struct {
	const char *uid;
	int32_t new_state; /* entelea_ch_state_t value */
} entelea_safe_state_evt_t;

/* ---- Driver dispatch (single call site; source-policy gate) ---- */

/* The driver ops table is a private implementation of the safety module
 * (internal.h) and is not declared publicly: exposing it would create a bypass
 * write entry that skips commit, clamping, and estop (review defect 1).
 * External read-only observation goes through entelea_driver_sim_writes. The
 * board-level backend assembly goes through entelea_drv_*_init below. A
 * mechanical check forbids references to entelea_drivers outside the module.
 * Write functions return 0 or a negative errno: a backend failure must
 * propagate to the commit result (defect 2). */

/* Write-sequence record of the stub driver (golden data source; the real
 * hardware driver path records the same way). */
typedef struct {
	uint64_t t_ms;
	uint32_t value_u; /* encoding as in entelea_value_encode */
} entelea_write_rec_t;

/** [thread] Copies the channel's write-sequence snapshot (oldest to newest).
 * Returns the actual number of entries. */
size_t
entelea_driver_sim_writes(const char *uid, entelea_write_rec_t *out,
			  size_t max);

/* ---- Test hooks (ENTELEA_TEST builds only) ---- */
#ifdef ENTELEA_TEST
/** Reason of the most recent system_fail (0 = none occurred). */
uint32_t
entelea_safety_test_fail_reason(void);
/** Clears the channel table, latches, and link state (test isolation;
 * unreachable in production). Also resets the backend ready flags. */
void
entelea_safety_test_reset(void);
/** Resets only the backend ready flag and failure count (used by not-ready
 * negative tests). */
void
entelea_drv_test_reset(void);
#endif

/* ---- Failure observability (no silent backend or clock failures) ---- */

/** [any] Cumulative driver write failures. On the force and estop paths, which
 * cannot return from the ISR, failures are recorded here. On the commit and
 * poweron paths, failures propagate through the return value and are also
 * counted here. */
uint32_t
entelea_safety_force_write_failures(void);

/** [boot/board] Initializes the real-hardware PWM output backend (board
 * assembly surface; driver_dispatch.c). Depends on the ENTELEA_DRV_PWM compile
 * definition and a zephyr,user node (pwm-uid + pwms). If it is not enabled or
 * the binding is missing, returns -ENODEV. */
int
entelea_drv_pwm_init(void);

/** [any] PWM backend write failure count (observation surface). */
uint32_t
entelea_drv_pwm_err_count(void);

/** [boot/board] Initializes the real-hardware GPIO output backend
 * (ENTELEA_DRV_GPIO plus the uid and io-gpios of zephyr,user). If it is not
 * enabled or the binding is missing, returns -ENODEV. */
int
entelea_drv_gpio_init(void);

/** [any] Cumulative module clock read failures. The guard keeps the last valid
 * value on failure or backward steps. */
uint32_t
entelea_safety_clock_faults(void);

/** [any] Cumulative audit writes dropped by throttling (observed during 1 Hz
 * storm sampling). */
uint32_t
entelea_safety_audit_throttled(void);

/* ---- Control-source arbitration (R04; same form as EN 60204-1 9.2) ---- */

typedef enum {
	ENTELEA_ARB_REMOTE = 0,
	ENTELEA_ARB_LOCAL = 1
} entelea_arb_owner_t;

/* Write-path source tags: APP = carrier of orchestration intent (natives);
 * REMOTE = remote orchestration writes (the only surface the suppression
 * gate acts on); LOCAL = local interactive writes (future local UI). */
typedef enum {
	ENTELEA_SRC_APP = 0,
	ENTELEA_SRC_REMOTE = 1,
	ENTELEA_SRC_LOCAL = 2,
} entelea_commit_src_t;

/** [thread] Changes the seat: only the gate bit changes, and
 * ENTELEA_EVT_ARB_CHANGED is published. No channel is written; output
 * continuity is carried by APP logic. Idempotent: the same seat produces no
 * event. */
entelea_result_t
entelea_safety_set_arb(entelea_arb_owner_t owner);

/** [any] Current seat owner. */
entelea_arb_owner_t
entelea_safety_arb_owner(void);

/** [thread] Source-tagged write path. While the local seat is active, REMOTE
 * sources are refused (ENTELEA_E_STATE is recorded); APP and LOCAL are not
 * gated. */
entelea_result_t
entelea_safety_commit_src(const char *uid, entelea_out_value_t v,
			  entelea_commit_src_t src);

/* ARB event payload. */
typedef struct {
	uint8_t owner; /* entelea_arb_owner_t, new seat owner */
	uint8_t old; /* previous owner (no event if unchanged) */
} entelea_arb_evt_t;

/* Capacity source: DEC-27 (legacy LLD-ts-safety sections 2 and 7). R02 uses
 * compile-time constants. */
#define ENTELEA_SAFETY_MAX_CHANNELS 32
/* Per-channel write-sequence ring capacity (structural value). */
#define ENTELEA_SAFETY_SIM_REC_PER_CH 32

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_SAFETY_H */
