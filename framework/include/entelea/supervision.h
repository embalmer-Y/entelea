/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_SUPERVISION_H
#define ENTELEA_SUPERVISION_H

/* R02 supervision boot contract: see framework/runtime/supervision/contract.md.
 * Supervision is split from the business boot: this module only provides the
 * sequential executor and fail-safe semantics. The step table is assembled by
 * the platform (platform/zephyr/boot_steps.c). The order is the specification:
 * steps may only be appended at the tail; the existing order must not
 * change. */

#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef entelea_result_t (*entelea_boot_step_fn)(void);

typedef struct {
	const char *name;
	entelea_boot_step_fn fn;
} entelea_boot_step_t;

/* fail-safe reason encoding, following the legacy TS_FAIL_* bit layout
 * (source domain << 16 | payload). */
#define ENTELEA_FAIL_SRC_BOOT 0x0001U
#define ENTELEA_FAIL_SRC_WDT 0x0002U
#define ENTELEA_FAIL_BOOT(idx) \
	((uint32_t) ENTELEA_FAIL_SRC_BOOT << 16 | (uint32_t) (idx))
#define ENTELEA_FAIL_WDT(src) \
	((uint32_t) ENTELEA_FAIL_SRC_WDT << 16 | (uint32_t) (src))

/** [thread] Runs the steps in fixed order. After each step completes, publishes
 * ENTELEA_EVT_BOOT_STEP (including the failed step's result). If any step
 * fails, calls entelea_safety_system_fail(ENTELEA_FAIL_BOOT(i)) and then halts
 * in a controlled way: it stops feeding the WDT so the hard WDT reset acts as
 * the fallback. ENTELEA_TEST builds may return from the halt path, and tests
 * assert on that. When all steps succeed, publishes ENTELEA_EVT_BOOT_DONE and
 * returns; the caller (main) then moves to resident supervision idle. */
void
entelea_supervision_boot(const entelea_boot_step_t *steps, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_SUPERVISION_H */
