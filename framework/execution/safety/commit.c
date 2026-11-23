/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The single exit point of the write path (migrated from the old
 * safety/commit.c; R02 contract: framework/execution/safety/contract.md).
 * Internal flow: table lookup -> ACTIVE check -> clamp -> slew -> current
 * limit -> final critical section (irq_lock re-checks forced, so an estop ISR
 * racing this thread's final write is caught).
 * Relative to the old implementation: every rejected commit (each early
 * return) is persisted through the R03 audit sink, and control-source
 * arbitration arrived with R04 (set_arb in channel.c, commit_src here).
 * commit_locked is kept for the R04 atomic budget check-and-commit path.
 */
#include <string.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include "internal.h"

static struct k_mutex commit_lock = Z_MUTEX_INITIALIZER(commit_lock);

void
entelea_safety_write_lock(void)
{
	__ASSERT(!k_is_in_isr(), "write lock is thread-only");
	(void) k_mutex_lock(&commit_lock, K_FOREVER);
}

void
entelea_safety_write_unlock(void)
{
	(void) k_mutex_unlock(&commit_lock);
}

static entelea_result_t
commit_impl(const char *uid, entelea_out_value_t v);

entelea_result_t
entelea_safety_commit(const char *uid, entelea_out_value_t v)
{
	if (k_is_in_isr()) {
		return ENTELEA_E_PERM; /* [thread] API (no ISR entry) */
	}
	entelea_safety_write_lock();
	entelea_result_t r = commit_impl(uid, v);
	entelea_safety_write_unlock();
	return r;
}

entelea_result_t
entelea_safety_commit_locked(const char *uid, entelea_out_value_t v,
			     struct entelea_safety_pend *pend)
{
	/* Caller already holds write_lock (R04 atomic budget check-and-commit
	 * path). R02 has no persistent payload; pend only keeps the contract
	 * shape. */
	ARG_UNUSED(pend);
	return commit_impl(uid, v);
}

/* Core: every early-return path is audited by the outer wrapper
 * (exception class). */
static entelea_result_t
commit_impl(const char *uid, entelea_out_value_t v);

/* Suppression gate (R04): while the local seat is active, remote
 * orchestration writes are rejected (identity-level rejection, before
 * channel-state semantics; logged to audit). */
static entelea_result_t
commit_src_gate(entelea_commit_src_t src)
{
	if (src == ENTELEA_SRC_REMOTE &&
	    (entelea_arb_owner_t) atomic_get(&entelea_arb_owner) ==
		    ENTELEA_ARB_LOCAL) {
		return ENTELEA_E_STATE;
	}
	return ENTELEA_OK;
}

static entelea_result_t
commit_run(int i, entelea_ch_kind_t kind, entelea_out_value_t v)
{
	(void) kind; /* Audit trimmed (R03); R04 passes src via wrapper */
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	const entelea_out_channel_t *ch = s->desc;

	if (s->state != ENTELEA_ST_ACTIVE) {
		return ENTELEA_E_STATE; /* Writes rejected in safe state */
	}
	if (atomic_get(&entelea_forced) != 0) {
		return ENTELEA_E_STATE; /* fail-safe latch active */
	}

	entelea_result_t res = ENTELEA_OK;

	/* 1) Clamp to [min,max]; interception returns ENTELEA_E_RANGE (clamp,
	 *    slew and budget all count as "output intercepted by the safety
	 *    layer"). */
	if (ch->kind == ENTELEA_CH_PWM) {
		if (v.u < ch->limits.min) {
			v.u = ch->limits.min;
			res = ENTELEA_E_RANGE;
		} else if (v.u > ch->limits.max) {
			v.u = ch->limits.max;
			res = ENTELEA_E_RANGE;
		}
	}

	/* 2) Slew: the total budget is anchored to the last confirmed
	 *    write (value = shadow, time = last_t). Bound = slew * (now -
	 *    last_t), with no +1 free jump: the total change across commits
	 *    repeated within the same millisecond is at most slew
	 *    (third-round review finding 4; a per-commit dt+1 let 200
	 *    same-kind commits bypass the rate limit). A split to the bound
	 *    returns ENTELEA_E_RANGE (the actual value is observable via
	 *    readback). A successful write refreshes the anchor (inside the
	 *    final critical section); later commits are measured from the
	 *    new anchor.
	 *    Clock gate: a channel with a slew limit requires this read to
	 *    succeed for both the first commit (baseline set-up) and later
	 *    commits; unknown time must not set a baseline or establish a
	 *    bound. Overflow: the bound saturates at the u32 limit
	 *    (practically unreachable with u64 ms timestamps; kept only for
	 *    determinism, staying within the u32 domain). */
	if (ch->limits.slew_per_ms > 0) {
		uint64_t now;

		if (!entelea_safety_now_ms_checked(&now)) {
			return ENTELEA_E_STATE; /* Time unknown: no bound set */
		}
		if (s->have_last) {
			if (now < s->last_t) {
				return ENTELEA_E_STATE; /* Clock anomaly */
			}
			uint64_t dt = now - s->last_t;
			uint64_t bound64 =
				(uint64_t) ch->limits.slew_per_ms * dt;
			uint32_t bound = bound64 > 0xFFFFFFFFU ?
						 0xFFFFFFFFU :
						 (uint32_t) bound64;
			uint32_t last_u =
				entelea_value_encode(ch->kind, s->shadow);
			uint32_t req_u = entelea_value_encode(ch->kind, v);

			if (req_u > last_u &&
			    (uint64_t) (req_u - last_u) > bound) {
				v.u = last_u + bound;
				res = ENTELEA_E_RANGE;
			} else if (req_u < last_u &&
				   (uint64_t) (last_u - req_u) > bound) {
				v.u = last_u - bound;
				res = ENTELEA_E_RANGE;
			}
		}
		/* !have_last: the channel has had no confirmed write since
		 * registration (at real boot, poweron_init already set the
		 * baseline, so this branch covers only the no-anchor case).
		 * The first commit is bounded only by min/max/clamp; the write
		 * itself becomes a confirmed write and refreshes the anchor. */
	}

	/* 3) Current limit (POWER): reject the whole commit if the requested
	 *    current exceeds the limit. */
	if (ch->kind == ENTELEA_CH_POWER && v.pwr.en &&
	    v.pwr.ma > ch->limits.current_limit_ma) {
		return ENTELEA_E_RANGE;
	}

	/* 4) Final critical section: guards against an estop ISR racing this
	 *    thread's final write. forced may be set between the pre-check and
	 *    this point: if not yet written, return ENTELEA_E_STATE; a backend
	 *    write failure returns ENTELEA_E_IO (defect 2: a backend failure
	 *    must never update the shadow or report success). Neither case
	 *    keeps the slew baseline. */
	unsigned int key = irq_lock();
	bool written = false;

	if (atomic_get(&entelea_forced) == 0) {
		if (entelea_drivers[ch->kind].write(ch, &v) == 0) {
			s->shadow = v;
			s->last_t = entelea_safety_now_ms();
			s->have_last = true;
			written = true;
		}
	}
	irq_unlock(key);
	if (!written) {
		entelea_safety_count_write_failure();
		res = (atomic_get(&entelea_forced) != 0) ? ENTELEA_E_STATE :
							   ENTELEA_E_IO;
	}
	return res;
}

/* Outer layer: table lookup and exception audit, covering every early
 * return (E_NOTFOUND, safe state, clock gate, etc.). */
entelea_result_t
entelea_safety_commit_src(const char *uid, entelea_out_value_t v,
			  entelea_commit_src_t src)
{
	if ((int) src < 0 || src > ENTELEA_SRC_LOCAL) {
		return ENTELEA_E_PARAM;
	}
	if (k_is_in_isr()) {
		return ENTELEA_E_PERM; /* [thread] API */
	}
	if (commit_src_gate(src) != ENTELEA_OK) {
		/* Suppression logged (reuses the exception audit surface). */
		int i = entelea_ch_find(uid);

		if (i >= 0) {
			const entelea_storage_audit_entry_t e = {
				.t_ms = entelea_safety_now_ms(),
				.ch_idx = (uint16_t) i,
				.kind = (uint8_t) entelea_ch_table[i]
						.desc->kind,
				.res = ENTELEA_E_STATE,
				.value_u = entelea_value_encode(
					entelea_ch_table[i].desc->kind, v),
			};

			entelea_safety_audit_persist(&e, 0);
		}
		return ENTELEA_E_STATE;
	}
	/* Review P1 fix: same write_lock discipline as commit (previously a
	 * bare commit_impl call bypassed the lock; a race with set_link could
	 * physically write a product value to a SAFE_LINKLOSS channel). */
	entelea_safety_write_lock();
	entelea_result_t r = commit_impl(uid, v);
	entelea_safety_write_unlock();
	return r;
}

static entelea_result_t
commit_impl(const char *uid, entelea_out_value_t v)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	entelea_ch_kind_t kind = entelea_ch_table[i].desc->kind;
	entelea_result_t res = commit_run(i, kind, v);

	if (res != ENTELEA_OK) {
		const entelea_storage_audit_entry_t e = {
			.t_ms = entelea_safety_now_ms(),
			.ch_idx = (uint16_t) i,
			.kind = (uint8_t) kind,
			.res = res,
			.value_u = entelea_value_encode(kind, v),
		};

		entelea_safety_audit_persist(&e, 0);
	}
	return res;
}

entelea_result_t
entelea_safety_readback(const char *uid, entelea_out_value_t *out)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return ENTELEA_E_NOTFOUND;
	}
	*out = entelea_ch_table[i].shadow;
	return ENTELEA_OK;
}
