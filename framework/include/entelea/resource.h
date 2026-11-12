/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_RESOURCE_H
#define ENTELEA_RESOURCE_H

/* R04 resource budget contract: see framework/execution/resource/contract.md.
 * Power budget: the total comes from prov (read-only at runtime). Accounting is
 * the sum of each output channel's safety readback (single-source discipline;
 * no separate shadow ledger). Exceeding the budget does not derate online.
 * Check and commit are atomic: the budget check and the commit run in the same
 * safety write_lock critical section (reusing entelea_safety_commit_locked). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>
#include <entelea/safety.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t budget_ma;
	uint32_t used_ma;
	uint32_t peak_ma;
	uint16_t slots;
} entelea_power_snapshot_t;

/** [any] Budget total (read-only from prov). Unconfigured = 0 = all power
 * requests rejected (the default is the safe side). */
uint32_t
entelea_power_budget_ma(void);

/** [any] Current power use (sum of enabled channel readbacks; also updates
 * the peak). */
uint32_t
entelea_power_used_ma(void);

/** [any] Snapshot (observation surface). */
void
entelea_power_snapshot(entelea_power_snapshot_t *out);

/** [thread] Controlled power commit (atomic budget check and commit):
 * 1. SET gate (the principal must declare power:write).
 * 2. Inside the safety write_lock: check the budget (sum + request <= budget),
 *    then call entelea_safety_commit_locked (the final protection step remains
 *    inside it), then unlock.
 * Over budget = E_RANGE (the whole request is rejected; existing channels are
 * unaffected). The channel must be ACTIVE.
 * v.en=false (turn off) is not subject to the budget. */
entelea_result_t
entelea_power_request(entelea_principal_t ctx, const char *safety_uid,
		      entelea_out_value_t v);

#ifdef ENTELEA_TEST
void
entelea_power_test_set_budget(uint32_t budget_ma);
void
entelea_power_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_RESOURCE_H */
