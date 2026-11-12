/* SPDX-License-Identifier: Apache-2.0 */
/* R02 D12 hardware-gate acceptance image (board level): closed-loop
 * verification of the two-phase fault stop on real silicon.
 * Phase discrimination = hwinfo reset cause. The noinit RAM backend does not
 * survive a reset (BSS is reset on every boot), and the board flash backend
 * belongs to the R03 board batch, so neither is used for discrimination:
 *  P1 (entered on power-on or external hard reset): healthy window of 6 s
 *     (differential proof that patrol and MWDT feeding are running; without
 *     MWDT installed or fed, the image would not survive the window) ->
 *     production system_fail(FAIL_WDT 0xFF) -> stop feeding -> task_wdt
 *     channel expires after 5 s -> sys_reboot (expected reset cause =
 *     SOFTWARE, <= ~5 s).
 *  P2 (entered with reset cause = SOFTWARE): irq_lock + spin wedges the
 *     kernel (single-core board config; CONFIG_SMP unset) -> task_wdt monitor
 *     dies -> MWDT0 (RESET_SOC, window 3+2 s) expires -> SoC hard reset
 *     (expected reset cause = WATCHDOG; the ROM banner rst:0x7
 *     (HP_SYS_HP_WDT_RESET) independently corroborates).
 *  P3 (entered with reset cause = WATCHDOG): double closed loop complete ->
 *     resident (patrol keeps feeding the watchdog).
 * No test hooks (ENTELEA_TEST undefined): only production APIs are used. */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/hwinfo.h>
#include <entelea/safety.h>
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
		/* Pre-boot clock bind failure: fail-safe (as minimal). */
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0;
	}
	printk("[hw-gate] boot sequence begins\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
	/* Any boot step failure does not return (fail-safe halt, WDT fallback
	 * reset). Reaching this point means all five steps passed, including
	 * wdt_start installing successfully on the real MWDT0. */
	uint32_t cause = 0;
	int rc = hwinfo_get_reset_cause(&cause);

	printk("[hw-gate] boot ok (patrol=%ums channel=%ums); hwinfo rc=%d "
	       "cause=0x%x\n",
	       entelea_wdt_patrol_period_ms(), ENTELEA_WDT_CHANNEL_MS, rc,
	       cause);

	if (rc == 0 && cause == RESET_WATCHDOG) {
		printk("[hw-gate] P2 VERIFIED: hardware MWDT reset the SoC\n");
		printk("[hw-gate] gate complete: parking resident (patrol "
		       "feeding)\n");
	} else if (rc == 0 && cause == RESET_SOFTWARE) {
		printk("[hw-gate] P1 VERIFIED: system_fail -> channel expiry "
		       "-> reset\n");
		printk("[hw-gate] P2: wedging kernel now; MWDT reset expected "
		       "<=~8s\n");
		(void) irq_lock();
		volatile unsigned spin = 0;

		for (;;) {
			spin++;
		}
	} else {
		printk("[hw-gate] P1: healthy 6s window (alive = patrol+MWDT "
		       "fed)\n");
		for (int i = 1; i <= 6; i++) {
			k_sleep(K_SECONDS(1));
			printk("[hw-gate] alive %ds\n", i);
		}
		printk("[hw-gate] P1: system_fail(FAIL_WDT 0xff); reset "
		       "expected <=~5s\n");
		entelea_safety_system_fail(ENTELEA_FAIL_WDT(0xFFu));
	}
	for (;;) {
		k_sleep(K_FOREVER); /* idle; P1 feed-stop triggers reset */
	}
	return 0;
}
