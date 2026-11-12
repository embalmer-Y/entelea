/* SPDX-License-Identifier: Apache-2.0 */
/* R02 minimal supervision image: boots without the intelligence module,
 * stays safe, and explicitly turns outputs off.
 * Assembly order: bind instance clock -> supervision executor (estop ->
 * all-channel safe initial state -> WDT -> ready) -> resident idle. Business
 * steps are appended by later batches at the tail. */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"

static entelea_clock_t platform_clock;

int
main(void)
{
	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK) {
		/* Clock bind failure is a pre-boot failure: fail safe, do not
		 * half-start. Reason: BOOT(0), pre-step-0 assembly failure. */
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0; /* reached only in ENTELEA_TEST builds */
	}

	printk("[entelea] minimal supervision image: outputs held at safe "
	       "poweron\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
	{
		uint32_t reason = 0;
		bool present = false;

		entelea_storage_last_reset(&reason, &present);
		if (present) {
			printk("[entelea] last abnormal reset reason=0x%08x "
			       "(noinit)\n",
			       reason);
		}
	}
#ifdef ENTELEA_STORM_DEMO
	/* r16 storm demo variant (--cmake-arg=-DSTORM_DEMO=ON): forces an
	 * abnormal-reset loop: system_fail -> wdt stops feeding -> MWDT reset
	 * (cause=WATCHDOG, abnormal). After bootguard counts >=3,
	 * SAFE-HALT stays resident (feeding does not fail).
	 * Exit only via POR. */
	if (entelea_platform_guard_safe_halt()) {
		printk("[storm-demo] SAFE-HALT confirmed (business halted, "
		       "feeding)\n");
	} else {
		printk("[storm-demo] forcing abnormal reset in 3s...\n");
		k_sleep(K_SECONDS(3));
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(7));
	}
	k_sleep(K_FOREVER);
	return 0;
#endif
	if (entelea_platform_guard_safe_halt()) {
		/* RF1 G5: production path also consumes SAFE-HALT. This image
		 * has no business, so it only marks the state. Outputs are
		 * already safe; heartbeat and patrol continue. */
		printk("[guard] SAFE-HALT active: business disabled\n");
	}
	for (;;) {
		/* r14 hardware observation: a 5 s heartbeat prints the safety
		 * summary and estop latch (estop gate criteria line; the event
		 * itself is still re-sent by WDT patrol as EVT_ESTOP). */
		entelea_safety_summary_t sum;
		bool latched = entelea_safety_estop_latched();
		uint32_t at = entelea_safety_estop_latched_at();

		if (entelea_safety_summary(&sum) == ENTELEA_OK) {
			printk("[entelea] safety: ch=%u safe_fault=%u estop=%s"
			       " at=%ums lvl=%d\n",
			       sum.channels,
			       sum.by_state[ENTELEA_ST_SAFE_FAULT],
			       latched ? "LATCHED" : "clear", latched ? at : 0U,
			       entelea_safety_estop_level_raw());
		}
		k_sleep(K_SECONDS(5));
	}
	return 0;
}
