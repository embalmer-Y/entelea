/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R12 trial state machine and Forge integration contract (new-only; see
 * contract.md in this directory).
 * The programming loop is the OpenHands Software Agent SDK (owner-approved);
 * the SDK-side loop is not inside the firmware. This layer is the firmware
 * trial state machine (single candidate, object-bound confirmation, execution
 * gate, fail-closed timeout and recovery) plus factory artifact intake
 * (out-of-domain and unknown payloads are refused).
 * Engineering pass != user acceptance: confirm starts the trial; genuine user
 * authorization is the responsibility of the trusted caller.
 * Single-thread assumption: all entry points run on [thread] with no internal
 * lock (same thread as the wasm host).
 */
#include <string.h>
#include <entelea/appstore.h>
#include <entelea/delivery.h>
#include <entelea/principal.h>
#include <entelea/storage.h>

static entelea_trial_state_t state;
static entelea_trial_candidate_t current;
static uint32_t last_epoch; /* Epoch assigned by the last submit (0 = never) */
static bool initialized;

static entelea_result_t
ensure_initialized(void)
{
	return initialized ? ENTELEA_OK : entelea_trial_restore();
}

static void
clear_candidate(void)
{
	memset(&current, 0, sizeof(current));
}

/* State check (no state change): refusal order matches the old submit. */
static entelea_result_t
admit_check(uint8_t slot)
{
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN) {
		return ENTELEA_E_STATE;
	}
	if (state == ENTELEA_TRIAL_ACTIVE) {
		return ENTELEA_E_BUSY; /* ACTIVE: no new candidates */
	}
	if (state == ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_BUSY; /* PENDING: shown object is immutable */
	}
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot != ENTELEA_TRIAL_SLOT_UNKNOWN &&
	    slot != current.slot) {
		return ENTELEA_E_BUSY; /* Slot reserved: same slot only */
	}
	if (last_epoch == UINT32_MAX) {
		return ENTELEA_E_RANGE; /* No wraparound reuse of old objects */
	}
	return ENTELEA_OK;
}

static bool
perm_valid(const char *perm, uint8_t len)
{
	entelea_perm_table_t t;

	if (len > ENTELEA_TRIAL_PERM_MAX) {
		return false;
	}
	entelea_perm_table_init(&t);
	return entelea_perm_parse_list(perm, len, &t) == ENTELEA_OK;
}

static bool
budget_valid(uint32_t steps)
{
	return steps >= 1U && steps <= ENTELEA_TRIAL_STEPS_MAX;
}

static void
admit_commit(const entelea_trial_candidate_t *c)
{
	current = *c;
	last_epoch++;
	current.epoch = last_epoch;
	state = ENTELEA_TRIAL_PENDING_CONFIRM;
}

/* Device computes the candidate identity: the whole-file digest after
 * verification; when expect is non-NULL it must match. */
static entelea_result_t
admit_from_slot(const entelea_trial_request_t *req, const uint8_t *expect)
{
	entelea_tsap_view_t v;
	entelea_trial_candidate_t c;
	entelea_result_t r;

	if (req == NULL || req->slot >= ENTELEA_APP_SLOT_COUNT ||
	    !perm_valid(req->perm, req->perm_len)) {
		return ENTELEA_E_PARAM;
	}
	if (!budget_valid(req->budget_steps)) {
		return ENTELEA_E_RANGE;
	}
	r = admit_check(req->slot);
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_pkg_verify(req->slot, NULL, &v, NULL);
	if (r != ENTELEA_OK) {
		return r; /* No valid package in slot: no state change */
	}
	memset(&c, 0, sizeof(c));
	r = entelea_pkg_file_digest(req->slot, v.pkg_len, c.digest);
	if (r != ENTELEA_OK) {
		return r;
	}
	if (expect != NULL && memcmp(c.digest, expect, sizeof(c.digest)) != 0) {
		return ENTELEA_E_INVALID_SIG; /* digest != factory declared */
	}
	memcpy(c.perm, req->perm, req->perm_len);
	c.perm_len = req->perm_len;
	c.budget_steps = req->budget_steps;
	c.version = req->version;
	c.slot = req->slot;
	admit_commit(&c);
	return ENTELEA_OK;
}

entelea_result_t
entelea_trial_submit(const entelea_trial_request_t *req)
{
	return admit_from_slot(req, NULL);
}

/* Shown object must equal the current candidate field by field (shared by
 * confirm and promote). */
static bool
candidate_matches(const entelea_trial_candidate_t *shown)
{
	return memcmp(shown->digest, current.digest, sizeof(current.digest)) ==
		       0 &&
	       shown->perm_len == current.perm_len &&
	       memcmp(shown->perm, current.perm, current.perm_len) == 0 &&
	       shown->budget_steps == current.budget_steps &&
	       shown->version == current.version &&
	       shown->slot == current.slot && shown->epoch == current.epoch;
}

entelea_result_t
entelea_trial_confirm(const entelea_trial_candidate_t *shown)
{
	if (shown == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	if (!candidate_matches(shown)) {
		return ENTELEA_E_PERM; /* Stale/mismatch: stay PENDING */
	}
	state = ENTELEA_TRIAL_ACTIVE; /* User confirm: only promotion path */
	return ENTELEA_OK;
}

entelea_result_t
entelea_trial_reject(void)
{
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	state = ENTELEA_TRIAL_REJECTED; /* Slot kept (resubmittable) */
	return ENTELEA_OK;
}

entelea_result_t
entelea_trial_timeout(void)
{
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_PENDING_CONFIRM) {
		return ENTELEA_E_STATE;
	}
	state = ENTELEA_TRIAL_TIMEOUT; /* fail-closed revoke (slot kept) */
	return ENTELEA_OK;
}

entelea_result_t
entelea_trial_end(void)
{
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_ACTIVE) {
		return ENTELEA_E_STATE;
	}
	clear_candidate(); /* Epoch count kept (monotonic across candidates) */
	state = ENTELEA_TRIAL_NONE;
	return ENTELEA_OK;
}

entelea_trial_state_t
entelea_trial_state(void)
{
	return initialized ? state : ENTELEA_TRIAL_UNINITIALIZED;
}

void
entelea_trial_current(entelea_trial_candidate_t *out)
{
	if (out != NULL) {
		if (initialized) {
			*out = current;
		} else {
			memset(out, 0, sizeof(*out));
			out->slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
		}
	}
}

entelea_result_t
entelea_trial_gate(uint8_t slot, const uint8_t digest[32])
{
	if (digest == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state == ENTELEA_TRIAL_NONE ||
	    (current.slot != ENTELEA_TRIAL_SLOT_UNKNOWN &&
	     slot != current.slot)) {
		return ENTELEA_OK; /* Not governed by trial (see D05) */
	}
	if (state == ENTELEA_TRIAL_ACTIVE &&
	    memcmp(digest, current.digest, sizeof(current.digest)) == 0) {
		return ENTELEA_OK; /* Only the bytes the user confirmed */
	}
	return ENTELEA_E_PERM;
}

/* Grant lookup: grants the confirmed permissions and steps only when ACTIVE
 * and both the slot and the digest match. */
entelea_result_t
entelea_trial_grant(uint8_t slot, const uint8_t digest[32],
		    entelea_trial_grant_t *out)
{
	if (digest == NULL || out == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_ACTIVE || current.slot != slot ||
	    memcmp(digest, current.digest, sizeof(current.digest)) != 0) {
		return ENTELEA_E_PERM;
	}
	memcpy(out->perm, current.perm, current.perm_len);
	out->perm_len = current.perm_len;
	out->budget_steps = current.budget_steps;
	return ENTELEA_OK;
}

/* ---- Factory intake (out-of-domain/unknown payload fail-closed) ---- */

entelea_result_t
entelea_factory_intake(const entelea_factory_candidate_t *fc, uint8_t slot)
{
	if (fc == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	if (fc->payload_kind > 1) {
		return ENTELEA_E_PARAM; /* No wasm default; no suffix guess */
	}
	if (fc->budget_steps == 0 || fc->budget_steps > 10000000U ||
	    fc->mem_quota_kb == 0 || fc->mem_quota_kb > 512 ||
	    fc->permissions_len > ENTELEA_TRIAL_PERM_MAX) {
		return ENTELEA_E_RANGE; /* Budget/perm out of domain (R05) */
	}
	entelea_trial_request_t req;

	memset(&req, 0, sizeof(req));
	req.slot = slot;
	req.version = fc->runtime_version;
	req.budget_steps = fc->budget_steps;
	memcpy(req.perm, fc->permissions, fc->permissions_len);
	req.perm_len = fc->permissions_len;
	return admit_from_slot(&req, fc->package_digest);
}

/* ---- Cross-reboot persistence (board batch A-4; RF1 v3): record layout
 * (little-endian):
 * u8 ver=3 | u8 state | u8 cand_version | u8 slot | u32 last_epoch |
 * u32 budget_steps | u8 perm_len | perm[96] (zero-filled) | digest[32].
 * v1/v2 are not migrated: treated as bad records (fail-closed). */
#define TRIAL_PERSIST_VER 4U
#define TRIAL_REC_PERM_OFF 13U
#define TRIAL_REC_DIG_OFF (TRIAL_REC_PERM_OFF + ENTELEA_TRIAL_PERM_MAX)
#define TRIAL_REC_LEN (TRIAL_REC_DIG_OFF + 32U)
/* Per-slot stable items are appended to the same dual-copy meta record:
 * present|digest|budget|plen|perm. */
#define STABLE_ITEM_LEN (1U + 32U + 4U + 1U + ENTELEA_TRIAL_PERM_MAX)
#define PERSIST_REC_LEN \
	(TRIAL_REC_LEN + ENTELEA_APP_SLOT_COUNT * STABLE_ITEM_LEN)

typedef struct {
	bool present;
	uint8_t digest[32];
	entelea_trial_grant_t grant;
} stable_entry_t;

static stable_entry_t stable[ENTELEA_APP_SLOT_COUNT];

static void
put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

static uint32_t
get_le32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

entelea_result_t
entelea_trial_flush(void)
{
	uint8_t rec[PERSIST_REC_LEN];
	uint8_t verify[PERSIST_REC_LEN];
	uint16_t verify_len = 0;
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_NONE &&
	    current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN) {
		return ENTELEA_E_STATE;
	}

	memset(rec, 0, sizeof(rec));
	rec[0] = TRIAL_PERSIST_VER;
	rec[1] = (uint8_t) state;
	rec[2] = current.version;
	rec[3] = current.slot;
	put_le32(rec + 4, last_epoch);
	put_le32(rec + 8, current.budget_steps);
	rec[12] = current.perm_len;
	memcpy(rec + TRIAL_REC_PERM_OFF, current.perm, current.perm_len);
	memcpy(rec + TRIAL_REC_DIG_OFF, current.digest, sizeof(current.digest));
	for (uint8_t i = 0; i < ENTELEA_APP_SLOT_COUNT; i++) {
		uint8_t *p = rec + TRIAL_REC_LEN + i * STABLE_ITEM_LEN;

		p[0] = stable[i].present ? 1U : 0U;
		memcpy(p + 1, stable[i].digest, sizeof(stable[i].digest));
		put_le32(p + 33, stable[i].grant.budget_steps);
		p[37] = stable[i].grant.perm_len;
		memcpy(p + 38, stable[i].grant.perm, stable[i].grant.perm_len);
	}
	r = entelea_storage_meta_write(rec, sizeof(rec));
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_storage_meta_write(rec, sizeof(rec));
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_storage_meta_read_mirrored(verify, sizeof(verify),
					       &verify_len);
	if (r != ENTELEA_OK || verify_len != sizeof(rec) ||
	    memcmp(rec, verify, sizeof(rec)) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

/* ---- Stable records (D-E): one item per slot, persisted atomically with
 * the trial in the same dual-copy meta record. */

entelea_result_t
entelea_trial_promote(const entelea_trial_candidate_t *shown)
{
	if (shown == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (state != ENTELEA_TRIAL_ACTIVE) {
		return ENTELEA_E_STATE;
	}
	if (!candidate_matches(shown)) {
		return ENTELEA_E_PERM;
	}
	const entelea_trial_candidate_t keep = current;
	const uint8_t slot = current.slot;
	const stable_entry_t old = stable[slot];

	stable[slot].present = true;
	memcpy(stable[slot].digest, current.digest, sizeof(current.digest));
	stable[slot].grant.budget_steps = current.budget_steps;
	stable[slot].grant.perm_len = current.perm_len;
	memcpy(stable[slot].grant.perm, current.perm, current.perm_len);
	clear_candidate();
	state = ENTELEA_TRIAL_NONE;
	r = entelea_trial_flush(); /* End + stable item: one atomic write */
	if (r != ENTELEA_OK) {
		current = keep;
		stable[slot] = old;
		state = ENTELEA_TRIAL_ACTIVE;
	}
	return r;
}

entelea_result_t
entelea_stable_revoke(uint8_t slot)
{
	entelea_result_t r;

	if (slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	r = ensure_initialized();
	if (r != ENTELEA_OK) {
		return r; /* Persistent read failed: state closed as TIMEOUT;
			   * maintenance recovery needed */
	}
	if (state == ENTELEA_TRIAL_ACTIVE && current.slot == slot) {
		clear_candidate();
		state = ENTELEA_TRIAL_NONE;
	}
	/* Revoke in memory first (fail-closed). If the write fails, the
	 * outcome is unknown and must be retried. */
	stable[slot].present = false;
	memset(stable[slot].digest, 0, sizeof(stable[slot].digest));
	memset(&stable[slot].grant, 0, sizeof(stable[slot].grant));
	return entelea_trial_flush();
}

entelea_result_t
entelea_stable_grant(uint8_t slot, const uint8_t digest[32],
		     entelea_trial_grant_t *out)
{
	if (digest == NULL || out == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	memset(out, 0, sizeof(*out));
	entelea_result_t r = ensure_initialized();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (!stable[slot].present || memcmp(digest, stable[slot].digest,
					    sizeof(stable[slot].digest)) != 0) {
		return ENTELEA_E_PERM;
	}
	*out = stable[slot].grant;
	return ENTELEA_OK;
}

static entelea_result_t
meta_is_erased(bool *erased)
{
	uint8_t block[64];
	uint32_t size = entelea_storage_part_size(ENTELEA_PART_META);

	*erased = false;
	if (size != 2U * ENTELEA_STORAGE_META_REC) {
		return ENTELEA_E_IO;
	}
	for (uint32_t off = 0; off < size; off += sizeof(block)) {
		if (entelea_storage_read(ENTELEA_PART_META, off, block,
					 sizeof(block)) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		for (size_t i = 0; i < sizeof(block); i++) {
			if (block[i] != 0xFFU) {
				return ENTELEA_OK;
			}
		}
	}
	*erased = true;
	return ENTELEA_OK;
}

entelea_result_t
entelea_trial_restore(void)
{
	uint8_t rec[PERSIST_REC_LEN];
	uint16_t len = 0;
	entelea_result_t r;

	if (initialized && !(state == ENTELEA_TRIAL_TIMEOUT &&
			     current.slot == ENTELEA_TRIAL_SLOT_UNKNOWN)) {
		return ENTELEA_OK;
	}
	r = entelea_storage_meta_read_mirrored(rec, sizeof(rec), &len);

	clear_candidate();
	memset(stable, 0, sizeof(stable));
	initialized = true;
	if (r == ENTELEA_E_IO) {
		bool erased = false;

		if (meta_is_erased(&erased) == ENTELEA_OK && erased) {
			state = ENTELEA_TRIAL_NONE;
			return ENTELEA_OK;
		}
		state = ENTELEA_TRIAL_TIMEOUT;
		current.slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
		return ENTELEA_E_IO;
	}
	const bool whole = r == ENTELEA_OK && len == sizeof(rec);
	const uint32_t ep = whole ? get_le32(rec + 4) : 0U;
	const uint32_t budget = whole ? get_le32(rec + 8) : 0U;
	const bool perm_ok =
		whole && rec[12] <= ENTELEA_TRIAL_PERM_MAX &&
		perm_valid((const char *) (rec + TRIAL_REC_PERM_OFF), rec[12]);
	bool stable_ok = whole;

	for (uint8_t i = 0; whole && i < ENTELEA_APP_SLOT_COUNT; i++) {
		const uint8_t *p = rec + TRIAL_REC_LEN + i * STABLE_ITEM_LEN;

		stable_ok = stable_ok && p[0] <= 1U &&
			    (p[0] == 0U ||
			     (budget_valid(get_le32(p + 33)) &&
			      p[37] <= ENTELEA_TRIAL_PERM_MAX &&
			      (p[37] == 0U ||
			       perm_valid((const char *) (p + 38), p[37]))));
	}
	const bool valid = stable_ok && rec[0] == TRIAL_PERSIST_VER &&
			   rec[1] <= ENTELEA_TRIAL_TIMEOUT &&
			   (rec[1] == ENTELEA_TRIAL_NONE ||
			    (rec[3] < ENTELEA_APP_SLOT_COUNT && ep != 0U &&
			     budget_valid(budget) && perm_ok));

	if (!valid) {
		/* Inconsistent record: the candidate slot is unknown, so
		 * both slots are refused by the gate. */
		state = ENTELEA_TRIAL_TIMEOUT;
		current.slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
		return ENTELEA_E_IO;
	}
	if (ep > last_epoch) {
		last_epoch = ep; /* Monotonic: no epoch rollback */
	}
	state = (entelea_trial_state_t) rec[1];
	if (state != ENTELEA_TRIAL_NONE) {
		current.version = rec[2];
		current.slot = rec[3];
		current.epoch = ep;
		current.budget_steps = budget;
		current.perm_len = rec[12];
		memcpy(current.perm, rec + TRIAL_REC_PERM_OFF, rec[12]);
		memcpy(current.digest, rec + TRIAL_REC_DIG_OFF,
		       sizeof(current.digest));
	}
	for (uint8_t i = 0; i < ENTELEA_APP_SLOT_COUNT; i++) {
		const uint8_t *p = rec + TRIAL_REC_LEN + i * STABLE_ITEM_LEN;

		stable[i].present = p[0] == 1U;
		if (stable[i].present) {
			memcpy(stable[i].digest, p + 1,
			       sizeof(stable[i].digest));
			stable[i].grant.budget_steps = get_le32(p + 33);
			stable[i].grant.perm_len = p[37];
			memcpy(stable[i].grant.perm, p + 38, p[37]);
		}
	}
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
/* State machine test seam: accepts the caller's digest directly without
 * verification (must not be used on the production path). */
entelea_result_t
entelea_trial_submit_with_digest_for_test(const entelea_trial_candidate_t *cand)
{
	if (cand == NULL || cand->slot >= ENTELEA_APP_SLOT_COUNT ||
	    !perm_valid(cand->perm, cand->perm_len)) {
		return ENTELEA_E_PARAM;
	}
	if (!budget_valid(cand->budget_steps)) {
		return ENTELEA_E_RANGE;
	}
	entelea_result_t r = admit_check(cand->slot);

	if (r != ENTELEA_OK) {
		return r;
	}
	admit_commit(cand);
	return ENTELEA_OK;
}

void
entelea_trial_test_reset(void)
{
	state = ENTELEA_TRIAL_NONE;
	clear_candidate();
	memset(stable, 0, sizeof(stable));
	last_epoch = 0;
	initialized = false;
}
#endif
