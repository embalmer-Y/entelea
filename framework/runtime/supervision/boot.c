/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Supervised boot executor (migrated from the orchestration semantics of the
 * old core/boot.c; R02 contract: framework/runtime/supervision/contract.md).
 * Split adjustment: the step table is assembled by the platform (the old table
 * was compile-time concatenated in the same file as the implementation, mixing
 * business steps net/app/audit into core). The new executor only consumes the
 * table the platform passes in. The ordering spec (boot guard -> estop ->
 * all-channel safe start state -> WDT -> bindings ready, business appended at
 * the tail) is owned by platform/zephyr/boot_steps.c and guarded by ztest.
 * Any step failure -> system-wide fail-safe; never half-boot (old contract
 * item 6 semantics).
 * The executor does not print: it reports through the platform hooks below.
 */
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <zephyr/kernel.h>
#include <zephyr/toolchain.h> /* __weak */

__weak void
entelea_supervision_boot_report(entelea_boot_report_t what, unsigned idx,
				const char *name, entelea_result_t result)
{
	(void) what;
	(void) idx;
	(void) name;
	(void) result;
}

__weak bool
entelea_supervision_boot_halted(void)
{
	return false;
}

static void
publish_step(unsigned idx, entelea_result_t result, const char *name)
{
	/* Published after the step, with its result (even on failure): the
	 * replay observation point. */
	const entelea_boot_step_evt_t payload = {
		.idx = (uint8_t) idx,
		.result = result,
		.name = name,
	};
	const entelea_event_t evt = {
		.id = ENTELEA_EVT_BOOT_STEP,
		.data = &payload,
		.len = sizeof(payload),
	};

	entelea_evt_publish(&evt);
}

void
entelea_supervision_boot(const entelea_boot_step_t *steps, size_t count)
{
	bool halted = false;

	for (size_t i = 0; i < count; i++) {
		entelea_result_t res;

		if (halted &&
		    (steps[i].flags & ENTELEA_BOOT_STEP_SAFE_STATE) == 0) {
			entelea_supervision_boot_report(
				ENTELEA_BOOT_REPORT_SKIPPED, (unsigned) i,
				steps[i].name, ENTELEA_OK);
			continue;
		}
		res = steps[i].fn();
		publish_step((unsigned) i, res, steps[i].name);
		if (res != ENTELEA_OK) {
			/* Any step failure: system-wide fail-safe;
			 * never half-boot. Controlled halt: stop feeding
			 * the hard WDT, then fallback reset (noinit trace
			 * belongs to R03). */
			entelea_supervision_boot_report(
				ENTELEA_BOOT_REPORT_FAILED, (unsigned) i,
				steps[i].name, res);
			entelea_safety_system_fail(
				ENTELEA_FAIL_BOOT((uint32_t) i));
			for (;;) {
				k_sleep(K_FOREVER);
			}
		}
		if (!halted) {
			halted = entelea_supervision_boot_halted();
		}
	}

	if (halted) {
		return; /* SAFE-HALT: nothing booted, so no BOOT_DONE. */
	}
	const entelea_event_t done = {
		.id = ENTELEA_EVT_BOOT_DONE,
	};

	entelea_evt_publish(&done);
	/* On return, the caller (main) enters resident supervision idle. */
}
