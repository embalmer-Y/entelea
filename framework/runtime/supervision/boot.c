/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Supervised boot executor (migrated from the orchestration semantics of the
 * old core/boot.c; R02 contract: framework/runtime/supervision/contract.md).
 * Split adjustment: the step table is assembled by the platform (the old table
 * was compile-time concatenated in the same file as the implementation, mixing
 * business steps net/app/audit into core). The new executor only consumes the
 * table the platform passes in. The ordering spec (estop -> all-channel safe
 * start state -> WDT -> bindings ready, appended at the tail) is owned by
 * platform/zephyr/boot_steps.c and guarded by ztest.
 * Any step failure -> system-wide fail-safe; never half-boot (old contract
 * item 6 semantics).
 */
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <zephyr/kernel.h>

void
entelea_supervision_boot(const entelea_boot_step_t *steps, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		entelea_result_t res = steps[i].fn();
		/* Publish BOOT_STEP after each step, with its result (even on
		 * failure): the replay observation point. */
		const entelea_boot_step_evt_t payload = {
			.idx = (uint8_t) i,
			.result = res,
			.name = steps[i].name,
		};
		const entelea_event_t evt = {
			.id = ENTELEA_EVT_BOOT_STEP,
			.data = &payload,
			.len = sizeof(payload),
		};

		entelea_evt_publish(&evt);
		if (res != ENTELEA_OK) {
			/* Any step failure: system-wide fail-safe;
			 * never half-boot. Controlled halt: stop feeding
			 * the hard WDT, then fallback reset (noinit trace
			 * belongs to R03). */
			entelea_safety_system_fail(
				ENTELEA_FAIL_BOOT((uint32_t) i));
			for (;;) {
				k_sleep(K_FOREVER);
			}
		}
	}

	const entelea_event_t done = {
		.id = ENTELEA_EVT_BOOT_DONE,
	};

	entelea_evt_publish(&done);
	/* On return, the caller (main) enters resident supervision idle. */
}
