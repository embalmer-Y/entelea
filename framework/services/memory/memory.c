/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R10 experience memory (new-only N-MEMORY; see contract.md in this
 * directory). Bounded chain (full = the oldest yields, with a count).
 * Writes require consent (explicit consent: collection is not memory).
 * Deletion = tombstone interval (reads skip it; derived surfaces do not
 * revive). Recovery consistency = monotonic chain order (RAM backend,
 * in-process; the persistent backend follows the board-level batch).
 */
#include <string.h>
#include <entelea/memory.h>

static entelea_mem_entry_t chain[ENTELEA_MEM_CHAIN_MAX];
static uint64_t chain_seq[ENTELEA_MEM_CHAIN_MAX];
static uint32_t chain_len;
static uint32_t evicted_count;
static uint64_t write_seq;
/* Tombstone list (review P1 fix: a single tombstone overwritten by a second
 * delete would revive the first deleted interval, so this is now an
 * accumulating list). Capacity is bounded. Eviction discipline (plan
 * conformance review P2 fix: the old "overwrite the smallest from when full"
 * could revive deleted entries in the chain): only slots fully covered by
 * the new interval are reused, so coverage is never lost. With no empty slot
 * and no covered slot, the write fails with E_NOMEM (explicit refusal; no
 * deleted interval is revived). A full-range delete covers everything, so it
 * never fails due to fullness. */
#define ENTELEA_MEM_TOMBSTONES_MAX 8
static struct {
	uint64_t from, to;
	uint64_t through_seq;
	bool active;
} tombstones[ENTELEA_MEM_TOMBSTONES_MAX];
static uint32_t tombstoned_count;

static bool
is_tombstoned(uint64_t t, uint64_t seq)
{
	for (int i = 0; i < ENTELEA_MEM_TOMBSTONES_MAX; i++) {
		if (tombstones[i].active && t >= tombstones[i].from &&
		    t <= tombstones[i].to && seq <= tombstones[i].through_seq) {
			return true;
		}
	}
	return false;
}

entelea_result_t
entelea_mem_write(bool consent, const entelea_mem_entry_t *e)
{
	if (e == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (!consent) {
		return ENTELEA_E_PERM; /* No consent: not saved */
	}
	if (e->len == 0 || e->len > ENTELEA_MEM_ENTRY_MAX) {
		return ENTELEA_E_PARAM;
	}
	if (write_seq == UINT64_MAX) {
		return ENTELEA_E_RANGE;
	}
	if (chain_len == ENTELEA_MEM_CHAIN_MAX) {
		memmove(&chain[0], &chain[1],
			(chain_len - 1) * sizeof(chain[0]));
		memmove(&chain_seq[0], &chain_seq[1],
			(chain_len - 1) * sizeof(chain_seq[0]));
		chain_len--;
		evicted_count++; /* Bounded: oldest yields, no refusal */
	}
	chain[chain_len] = *e;
	chain_seq[chain_len++] = ++write_seq;
	return ENTELEA_OK;
}

size_t
entelea_mem_read(entelea_mem_entry_t *out, size_t max)
{
	if (out == NULL) {
		return 0;
	}
	size_t n = 0;

	for (uint32_t i = 0; i < chain_len && n < max; i++) {
		if (is_tombstoned(chain[i].t_ms, chain_seq[i])) {
			continue; /* Deleted entries stay deleted */
		}
		out[n++] = chain[i];
	}
	return n;
}

entelea_result_t
entelea_mem_delete(uint64_t t_from_ms, uint64_t t_to_ms)
{
	if (t_from_ms > t_to_ms) {
		return ENTELEA_E_PARAM;
	}
	/* Bounded, never revives: (1) an empty slot; (2) a slot fully covered
	 * by the new interval (coverage kept); (3) otherwise E_NOMEM, an
	 * explicit refusal that never overwrites a still-valid interval. */
	int slot = -1;
	int subsumed = -1;

	for (int i = 0; i < ENTELEA_MEM_TOMBSTONES_MAX; i++) {
		if (!tombstones[i].active) {
			slot = i;
			break;
		}
		if (subsumed < 0 && t_from_ms <= tombstones[i].from &&
		    tombstones[i].to <= t_to_ms) {
			subsumed = i; /* First covered slot; keep scanning */
		}
	}
	if (slot < 0) {
		slot = subsumed;
	}
	if (slot < 0) {
		return ENTELEA_E_NOMEM; /* Full, none covered: refuse */
	}
	tombstones[slot].from = t_from_ms;
	tombstones[slot].to = t_to_ms;
	tombstones[slot].through_seq = write_seq;
	tombstones[slot].active = true;
	tombstoned_count++;
	return ENTELEA_OK;
}

void
entelea_mem_health(entelea_mem_health_t *out)
{
	if (out == NULL) {
		return;
	}
	out->count = (uint16_t) chain_len;
	out->evicted = evicted_count;
	out->tombstoned = tombstoned_count;
}

#ifdef ENTELEA_TEST
entelea_result_t
entelea_mem_test_set_write_seq(uint64_t seq)
{
	if (chain_len != 0 || tombstoned_count != 0) {
		return ENTELEA_E_STATE;
	}
	write_seq = seq;
	return ENTELEA_OK;
}

void
entelea_mem_test_reset(void)
{
	memset(chain, 0, sizeof(chain));
	memset(chain_seq, 0, sizeof(chain_seq));
	chain_len = 0;
	evicted_count = 0;
	write_seq = 0;
	memset(tombstones, 0, sizeof(tombstones));
	tombstoned_count = 0;
}
#endif
