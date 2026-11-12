/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Power budget accounting and controlled power supply commit (migrated from
 * the old power/budget.c and the slots check-then-commit atomicity semantics;
 * R04 contract in contract.md of this directory).
 * Budget source = prov (read-only at runtime, contract 10); test injection is
 * only under ENTELEA_TEST.
 * Accounting source = safety readback (no shadow ledger; the physical
 * declaration takes precedence).
 * Check-then-commit atomicity: shares the same write_lock as safety commit
 * (lock order reused, no new lock).
 */
#include <string.h>
#include "internal.h"
#include <entelea/prov.h>
#include <entelea/resource.h>
#include <entelea/safety.h>

static uint32_t peak_ma;

#ifdef ENTELEA_TEST
static uint32_t test_budget_ma; /* Not built in prod; prov is the only source */
#endif

uint32_t
entelea_power_budget_ma(void)
{
#ifdef ENTELEA_TEST
	if (test_budget_ma != 0) {
		return test_budget_ma; /* Test injection (overrides prov) */
	}
#endif
	return entelea_prov()->power_budget_ma;
}

uint32_t
entelea_power_used_ma(void)
{
	uint32_t sum = 0;

	for (size_t i = 0; i < entelea_ch_count; i++) {
		const struct entelea_ch_slot *s = &entelea_ch_table[i];
		entelea_out_value_t rb;

		if (s->desc != NULL && s->desc->kind == ENTELEA_CH_POWER &&
		    entelea_safety_readback(s->desc->uid, &rb) == ENTELEA_OK &&
		    rb.pwr.en) {
			sum += rb.pwr.ma;
		}
	}
	if (sum > peak_ma) {
		peak_ma = sum;
	}
	return sum;
}

void
entelea_power_snapshot(entelea_power_snapshot_t *out)
{
	if (out == NULL) {
		return;
	}
	out->budget_ma = entelea_power_budget_ma();
	out->used_ma = entelea_power_used_ma();
	out->peak_ma = peak_ma;
	out->slots = 0;
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if (entelea_ch_table[i].desc != NULL &&
		    entelea_ch_table[i].desc->kind == ENTELEA_CH_POWER) {
			out->slots++;
		}
	}
}

/* SET gate: R04 controlled power requires a power:write declaration
 * (instance 0; single budget surface). */
static entelea_result_t
power_gate(entelea_principal_t ctx)
{
	return entelea_perm_check(ctx, ENTELEA_PERM_POWER, ENTELEA_PERM_WRITE,
				  0);
}

entelea_result_t
entelea_power_request(entelea_principal_t ctx, const char *safety_uid,
		      entelea_out_value_t v)
{
	if (safety_uid == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = power_gate(ctx);

	if (r != ENTELEA_OK) {
		return r;
	}
	/* Atomic check-then-commit (review P1 fix): the budget check and
	 * commit share one write_lock critical section, so concurrent
	 * power_request calls can no longer both pass the check and exceed
	 * the budget. */
	entelea_safety_write_lock();
	if (v.pwr.en && v.pwr.ma > 0) {
		uint32_t budget = entelea_power_budget_ma();
		uint32_t used = entelea_power_used_ma();
		uint32_t current = 0;
		entelea_out_value_t rb;

		if (entelea_safety_readback(safety_uid, &rb) == ENTELEA_OK &&
		    rb.pwr.en) {
			current = rb.pwr.ma; /* Replace: subtract own first */
		}
		if (v.pwr.ma > budget || used - current > budget - v.pwr.ma) {
			entelea_safety_write_unlock();
			return ENTELEA_E_RANGE; /* Reject all; no derating */
		}
	}
	r = entelea_safety_commit_locked(safety_uid, v, NULL);
	entelea_safety_write_unlock();
	return r;
}

#ifdef ENTELEA_TEST
void
entelea_power_test_set_budget(uint32_t budget_ma)
{
	test_budget_ma = budget_ma;
}

void
entelea_power_test_reset(void)
{
	test_budget_ma = 0;
	peak_ma = 0;
}
#endif
