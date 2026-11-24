/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_SUPERVISION_H
#define ENTELEA_SUPERVISION_H

/* R02 supervision boot contract: see framework/runtime/supervision/contract.md.
 * Supervision is split from the business boot: this module only provides the
 * sequential executor and fail-safe semantics. The step table is assembled by
 * the platform (platform/zephyr/boot_steps.c). The order is the specification:
 * new steps are appended at the tail, and the existing order changes only by
 * an owner decision (2026-11-24: the boot guard moved to the front). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef entelea_result_t (*entelea_boot_step_fn)(void);

/* Step class. A step carrying ENTELEA_BOOT_STEP_SAFE_STATE establishes or
 * upholds the safe state (outputs, watchdog, the boot guard itself) and runs
 * on every boot. A step without it provisions business: on a boot where the
 * boot guard has decided SAFE-HALT, such a step is not run. */
#define ENTELEA_BOOT_STEP_SAFE_STATE 0x01U

typedef struct {
	const char *name;
	entelea_boot_step_fn fn;
	uint8_t flags;
} entelea_boot_step_t;

/* fail-safe reason encoding, following the legacy TS_FAIL_* bit layout
 * (source domain << 16 | payload). */
#define ENTELEA_FAIL_SRC_BOOT 0x0001U
#define ENTELEA_FAIL_SRC_WDT 0x0002U
#define ENTELEA_FAIL_BOOT(idx) \
	((uint32_t) ENTELEA_FAIL_SRC_BOOT << 16 | (uint32_t) (idx))
#define ENTELEA_FAIL_WDT(src) \
	((uint32_t) ENTELEA_FAIL_SRC_WDT << 16 | (uint32_t) (src))

/* What the executor reports about a step that did not succeed. */
typedef enum {
	ENTELEA_BOOT_REPORT_FAILED, /* The step returned an error; the
				     * fail-safe follows. result is its value. */
	ENTELEA_BOOT_REPORT_SKIPPED, /* The step was not run: SAFE-HALT is in
				      * effect. result is ENTELEA_OK. */
} entelea_boot_report_t;

/** Reports a step that did not succeed. The platform layer prints it (the
 * executor itself stays silent); the default does nothing. */
void
entelea_supervision_boot_report(entelea_boot_report_t what, unsigned idx,
				const char *name, entelea_result_t result);

/** True once the boot guard has decided SAFE-HALT on this boot. The executor
 * asks after every step. The default is false. */
bool
entelea_supervision_boot_halted(void);

/** [thread] Runs the steps in fixed order. After each step that ran, publishes
 * ENTELEA_EVT_BOOT_STEP (including the failed step's result). If any step
 * fails, calls entelea_safety_system_fail(ENTELEA_FAIL_BOOT(i)) and then halts
 * in a controlled way: it stops feeding the WDT so the hard WDT reset acts as
 * the fallback. ENTELEA_TEST builds may return from the halt path, and tests
 * assert on that. When all steps succeed, publishes ENTELEA_EVT_BOOT_DONE and
 * returns; the caller (main) then moves to resident supervision idle.
 *
 * Once the boot guard reports SAFE-HALT, the steps that are not
 * ENTELEA_BOOT_STEP_SAFE_STATE are not run and publish nothing; such a boot
 * publishes no BOOT_DONE, because nothing booted. A step that keeps failing
 * would otherwise reset the device on every boot, and the guard's decision
 * could never take effect. */
void
entelea_supervision_boot(const entelea_boot_step_t *steps, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_SUPERVISION_H */
