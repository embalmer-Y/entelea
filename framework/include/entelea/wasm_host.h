/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_WASM_HOST_H
#define ENTELEA_WASM_HOST_H

/* R05 Wasm execution loop contract: see framework/services/package/contract.md
 * (section: Wasm host). WAMR pinned at 25bd7eb; fast interpreter, instruction
 * quota, WASI off, and pool mode (legacy DEC-17/25/31/48 #3 semantics). The
 * instance is bound to the R04 owner principal at load and cannot be replaced
 * afterwards. Natives decide only by owner: an integer passed in from Wasm
 * cannot be turned into a principal. owner={0} means unbound; natives are
 * always refused. Native authorization is an intersection: the trial-confirmed
 * permission list (only ACTIVE and byte-matching) and the owner's host
 * authorization must both allow it. A package without confirmation (NONE or
 * non-candidate) has no native permissions at all. The execution quota is
 * min(request, confirmed steps, 1e8). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_WASM_HEAP_SIZE 262144U /* pool; legacy DEC-27 256KB baseline */
#define ENTELEA_WASM_STACK_SIZE 4096U
#define ENTELEA_WASM_INSTRUCTIONS_PER_MS 1000U
#define ENTELEA_WASM_DEFAULT_INSTRUCTIONS 100000000U

/** [boot] Process-level WAMR init, once (pool mode injection). Repeated calls
 * are idempotent. If native registration fails, returns E_IO and rolls back
 * the runtime (it stays uninitialized and can be retried). */
entelea_result_t
entelea_wasm_init(void);

/** Verifies and loads the module in slot, binding the owner (V1: single
 * instance). If a live instance already exists, returns E_BUSY with no side
 * effects (stop it first). Verification and read errors propagate unchanged
 * (E_INVALID_SIG and E_IO do not overwrite each other). If the R12 trial gate
 * (whole-file sha256) refuses, returns E_PERM with no side effects. A module
 * that exceeds the load buffer returns E_RANGE. A failure does not write
 * app_id_out. */
entelea_result_t
entelea_wasm_load(uint8_t slot, entelea_principal_t owner,
		  uint32_t *app_id_out);

/** Calls exported function fn. ctx must equal the owner bound at load,
 * otherwise E_PERM. The trial gate is re-checked against the slot and digest
 * recorded at load; if it refuses, returns E_PERM (followed by the D12 gate).
 * The export ABI is only ()->i32 and (i32)->i32: the former requires arg to be
 * 0, and the latter receives arg. Any other signature returns E_PARAM. The
 * refusals above neither execute nor destroy the instance. The timeout in ms
 * is approximated by a step budget; on expiry, returns E_TIMEOUT after a
 * bounded stop (identified by the WAMR exception string "instruction limit").
 * Other traps return E_IO. Both destroy the instance. The budget is converted
 * at about 1000 instructions per ms and capped at 1e8. An explicit 0 means zero
 * instructions, not the default, and does not mean an actual wall-clock
 * deadline. */
entelea_result_t
entelea_wasm_call(uint32_t app_id, entelea_principal_t ctx, const char *fn,
		  int32_t arg, uint32_t timeout_ms, int32_t *ret_out);

/** Calls with the default 1e8 instruction budget. Other semantics are the same
 * as entelea_wasm_call. */
entelea_result_t
entelea_wasm_call_default(uint32_t app_id, entelea_principal_t ctx,
			  const char *fn, int32_t arg, int32_t *ret_out);

/** Stops and destroys the instance. The module is kept, following the WAMR
 * package-swap restart path. */
entelea_result_t
entelea_wasm_stop(uint32_t app_id);

/** Number of loaded instances (for observation). */
uint8_t
entelea_wasm_instance_count(void);

#ifdef ENTELEA_TEST
/* Stops the instance, unloads the module, and destroys the runtime; the next
 * init starts from scratch. */
void
entelea_wasm_test_reset(void);
/* Direct hook to the native decision, acting for the owner: grant is the
 * confirmed permission list (NULL = no confirmed grant). */
int32_t
entelea_wasm_test_probe_decide(entelea_principal_t owner, const char *grant);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_WASM_HOST_H */
