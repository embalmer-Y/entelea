/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_DELIVERY_H
#define ENTELEA_DELIVERY_H

/* R12 factory / trial / promotion contract: see
 * framework/services/delivery/contract.md.
 * The programming loop is the OpenHands Software Agent SDK (an owner-confirmed
 * decision); the SDK-side loop is not inside the firmware. The Forge-side
 * integration contract (a narrow interface) and the firmware trial state
 * machine live here. Single-candidate, content-bound trial confirmation and
 * stable-set admission (D-E) are implemented; promotion of trial data (D05
 * details) is not yet closed. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Trial state machine (single candidate slot; D05 direction) ---- */

typedef enum {
	ENTELEA_TRIAL_NONE = 0,
	ENTELEA_TRIAL_PENDING_CONFIRM, /* awaiting user hash/perm confirm */
	ENTELEA_TRIAL_ACTIVE, /* user confirmed; trial running */
	ENTELEA_TRIAL_REJECTED, /* user rejected; slot kept for re-submit */
	ENTELEA_TRIAL_TIMEOUT, /* confirm timed out; revoked (fail-closed) */
	ENTELEA_TRIAL_UNINITIALIZED, /* persistent state not yet read */
} entelea_trial_state_t;

#define ENTELEA_TRIAL_SLOT_UNKNOWN 0xFFU /* bad-record recovery: slot unknown */
#define ENTELEA_TRIAL_PERM_MAX 96U /* ';'-separated list, bytes excl. NUL */
#define ENTELEA_TRIAL_STEPS_MAX \
	100000000U /* confirmed step limit = device Wasm quota limit */

/* The object the user confirms: display = confirmation = execution binding.
 * Only the device computes the digest. */
typedef struct {
	uint8_t digest[32]; /* whole-file sha256 after verify (device) */
	char perm
		[ENTELEA_TRIAL_PERM_MAX]; /* e.g. "gpio:read:0"; empty = none */
	uint8_t perm_len;
	uint32_t
		budget_steps; /* confirmed steps; quota = min(req, this, cap) */
	uint8_t version;
	uint8_t slot; /* app slot holding the candidate */
	uint32_t epoch; /* submit epoch (set by submit; confirm echoes it) */
} entelea_trial_candidate_t;

/* Submit request: the host proposes only permissions, steps, and version.
 * It cannot specify the digest. */
typedef struct {
	uint8_t slot;
	uint8_t version;
	uint32_t budget_steps;
	char perm[ENTELEA_TRIAL_PERM_MAX];
	uint8_t perm_len;
} entelea_trial_request_t;

/* Permissions and steps granted by the execution gate. Valid only when
 * ACTIVE and the slot and digest match. */
typedef struct {
	char perm[ENTELEA_TRIAL_PERM_MAX];
	uint8_t perm_len;
	uint32_t budget_steps;
} entelea_trial_grant_t;

/** [thread] Submits a candidate (enters PENDING_CONFIRM). The device first
 * verifies the package in the slot, then uses that package's whole-file digest
 * as the candidate identity; the host cannot specify the digest. The permission
 * list must follow the entelea_perm_parse_list grammar (otherwise E_PARAM), and
 * the steps must be in 1..ENTELEA_TRIAL_STEPS_MAX (otherwise E_RANGE). In the
 * ACTIVE or PENDING state the result is BUSY (single candidate; the displayed
 * object cannot be replaced). In the REJECTED or TIMEOUT state, changing the
 * slot returns BUSY (the slot is kept, and re-submission must use the same
 * slot). If the slot has no valid package, returns the verification error and
 * leaves the state unchanged. If the epoch is exhausted, returns E_RANGE and
 * leaves the candidate unchanged. The first operation restores persistent
 * state first. */
entelea_result_t
entelea_trial_submit(const entelea_trial_request_t *req);

/** [thread] User confirmation (PENDING -> ACTIVE). shown is the object
 * displayed to the user and confirmed by them (taken from trial_current).
 * If any of the digest, permission list, steps, version, slot, or epoch
 * differs, returns E_PERM and stays PENDING. A stale display, or a
 * confirmation given after re-submission, has no effect. The caller is
 * responsible for real user authorization; this API does not verify user
 * identity or interaction. */
entelea_result_t
entelea_trial_confirm(const entelea_trial_candidate_t *shown);

/** [thread] User rejects (PENDING -> REJECTED). The slot is kept for
 * re-submission. */
entelea_result_t
entelea_trial_reject(void);

/** [thread] Timeout revocation (PENDING -> TIMEOUT; fail-closed). */
entelea_result_t
entelea_trial_timeout(void);

/** [thread] Ends the trial (ACTIVE -> NONE). The candidate slot is cleared
 * after promotion or abandonment. */
entelea_result_t
entelea_trial_end(void);

/** [any] State and current candidate (for observation). Before the first
 * restore, the state is UNINITIALIZED and the candidate slot is UNKNOWN. The
 * query itself does not read storage. The caller must serialize the state
 * machine and storage operations. */
entelea_trial_state_t
entelea_trial_state(void);
void
entelea_trial_current(entelea_trial_candidate_t *out);

/** [thread] Execution gate (queried by the wasm host before load or call).
 * digest is the slot's current whole-file sha256. A NONE state or a
 * non-candidate slot returns OK: the trial policy does not govern it, and this
 * pure check grants no native permissions (native grants are in grant, which
 * includes stable records). A candidate slot (or a candidate slot that is
 * unknown) returns OK only if ACTIVE and the digest matches; otherwise E_PERM
 * (PENDING, REJECTED, TIMEOUT, and bytes changed after confirmation are all
 * refused). */
entelea_result_t
entelea_trial_gate(uint8_t slot, const uint8_t digest[32]);

/** [thread] Grant query (called on every execution; not cached). Returns OK and
 * fills in the confirmed permission list and steps only when ACTIVE and both
 * the slot and digest match the confirmed object. Otherwise returns E_PERM, and
 * the caller must not grant any native permission. The grant becomes invalid at
 * the next query after the trial ends. */
entelea_result_t
entelea_trial_grant(uint8_t slot, const uint8_t digest[32],
		    entelea_trial_grant_t *out);

/** [thread] Promotes to stable (D-E). Runs only when ACTIVE and shown matches
 * every field of the current candidate. Ending the trial and the slot's stable
 * entry (the digest of the same bytes, the permission list, and the steps) are
 * persisted atomically in one two-copy meta write. On a write failure, returns
 * an error code and memory stays ACTIVE; when an error is returned, the
 * persistent result is whatever a restart reads, so the caller must re-confirm
 * or revoke. The caller must obtain a second user confirmation. */
entelea_result_t
entelea_trial_promote(const entelea_trial_candidate_t *shown);

/** [thread] Explicitly revokes the slot's stable entry. An ACTIVE trial in that
 * slot also ends. Revocation takes effect in memory first (fail-closed), and is
 * persisted in the same meta write. A result other than OK means the persistent
 * result is unknown; after a restart the slot may still hold native grants, so
 * retry until OK. */
entelea_result_t
entelea_stable_revoke(uint8_t slot);

/** [any] Stable-record grant (queried on every execution). If the slot's stable
 * record exists and the digest matches, grants its permissions and steps.
 * If the record is absent or revoked, or the digest differs, returns E_PERM and
 * zeroes the output. A persistent read failure returns E_IO and grants
 * nothing. */
entelea_result_t
entelea_stable_grant(uint8_t slot, const uint8_t digest[32],
		     entelea_trial_grant_t *out);

/* ---- Forge integration contract (narrow interface) ---- */

typedef struct {
	uint8_t package_digest
		[32]; /* whole-file sha256 of TSAP package (= device) */
	char permissions
		[ENTELEA_TRIAL_PERM_MAX]; /* permission list (as in request) */
	uint8_t permissions_len;
	uint8_t runtime_version; /* Wasm/Lua payload version */
	uint8_t payload_kind; /* 0=wasm 1=lua */
	uint32_t budget_steps; /* execution budget (factory range 1..10^7) */
	uint32_t mem_quota_kb; /* memory quota (factory range 1..512 KB) */
	char description[48]; /* candidate description (user-visible) */
} entelea_factory_candidate_t;

/** Firmware-side acceptance check for the factory artifact. After the device
 * verifies the package in the slot and computes its whole-file digest, the
 * digest must equal package_digest (otherwise E_INVALID_SIG, and nothing is
 * submitted). Out-of-range permission lists or budgets are refused. An unknown
 * payload_kind is refused (it is not interpreted by default). A slot out of
 * range returns E_PARAM. This only reaches PENDING and does not confirm on the
 * user's behalf. Budgets and permissions are not cross-checked against the
 * signed manifest (D-D: not signed in the first phase; constrained by user
 * confirmation and persistent records). */
entelea_result_t
entelea_factory_intake(const entelea_factory_candidate_t *fc, uint8_t slot);

/* ---- Restart persistence (board-level batch A-4; R03 meta) ---- */

/** [thread] Flushes state to persistent storage, including the NONE state, so
 * that clearing a slot is also persisted. Both copies are written and verified
 * to match; a failure at any step is reported. The assembly layer decides when
 * to call this; wear policy follows D05. */
entelea_result_t
entelea_trial_flush(void);

/** [boot] Restore: accepted only when both copies are valid and identical. If
 * the whole logical area is readable and fully erased, it counts as no record.
 * Otherwise, returns E_IO and sets state TIMEOUT and slot UNKNOWN (both slots
 * are refused by the gate). Once initialized, it is idempotent and does not
 * overwrite in-memory state. In UNKNOWN, an explicit retry of the read is
 * allowed. The first submit, execution gate, or flush also restores first; a
 * normal submit does not clear UNKNOWN, and the maintenance recovery method is
 * yet to be determined. A write must be followed by an explicit flush. */
entelea_result_t
entelea_trial_restore(void);

#ifdef ENTELEA_TEST
void
entelea_trial_test_reset(void);
/* State-machine test seam: accepts the caller's digest directly, without
 * verification, for state-machine test cases only. The production path (the
 * wasm suite) verifies the device-computed digest with a signed package. Field
 * validation is the same as submit. */
entelea_result_t
entelea_trial_submit_with_digest_for_test(
	const entelea_trial_candidate_t *cand);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_DELIVERY_H */
