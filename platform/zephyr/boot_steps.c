/* SPDX-License-Identifier: Apache-2.0 */
/* R02 platform assembly boot table: boot guard -> estop GPIO -> all-channel
 * safe initial state -> WDT -> supervision ready. Later business batches only
 * append steps at the tail.
 * Owner 2026-11-24: the boot guard runs first. A boot step that keeps failing
 * resets the device on every boot, and a guard that runs last never got to
 * count those resets. */
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain.h> /* __weak */
#include <entelea/guard.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"
#include "guard_ports.h"
#include <entelea/time.h>

/* D12 boot guard (board strong implementation: guard_bootguard.c; boards
 * without one, i.e. native_sim and tests, pass trivially). The SAFE-HALT
 * decision is consumed by the assembly layer via
 * entelea_platform_guard_safe_halt (the guard must not feed system_fail: that
 * would add an MWDT reset and become a storm source). */
#ifndef ENTELEA_REQUIRE_GUARD_BOOTGUARD
__weak bool
entelea_platform_guard_bootguard(void)
{
	return false;
}

__weak bool
entelea_platform_guard_safe_halt(void)
{
	return false;
}
#endif

/* Weak default for the persistent port: only targets linking guard_store.c
 * have the strong (KV) implementation. */
__weak entelea_result_t
entelea_platform_guard_store_init(entelea_guard_store_t *store)
{
	(void) store;
	return ENTELEA_E_UNINIT;
}

/* Weak board-level flash backend stub (R03 board batch): by default there is
 * no flash surface (RAM backend); targets linking
 * platform/zephyr/flash_store.c get the strong implementation. */
__weak entelea_result_t
entelea_platform_storage_mount_flash(void)
{
	return ENTELEA_E_UNINIT;
}

static entelea_result_t
mount_flash_backend(void)
{
	entelea_result_t m = entelea_platform_storage_mount_flash();

	/* Mount contract (review P2-2 fix): weak stub E_UNINIT means the
	 * target has no flash surface (legal passthrough to RAM); a strong
	 * implementation returning any other error means **flash configured
	 * but unusable**: the persistence guarantee is void. */
	return m == ENTELEA_E_UNINIT ? ENTELEA_OK : m;
}

/* Boot executor hooks. The framework does not print; these lines are what a
 * reset loop leaves on the console. */
void
entelea_supervision_boot_report(entelea_boot_report_t what, unsigned idx,
				const char *name, entelea_result_t result)
{
	if (what == ENTELEA_BOOT_REPORT_SKIPPED) {
		printk("[boot] step %u \"%s\" not run: SAFE-HALT\n", idx, name);
	} else {
		printk("[boot] step %u \"%s\" failed: %d -> fail-safe\n", idx,
		       name, result);
	}
}

bool
entelea_supervision_boot_halted(void)
{
	return entelea_platform_guard_safe_halt();
}

static entelea_result_t
step_guard_bootguard(void)
{
	static entelea_clock_t guard_clock; /* own uptime clock (idempotent) */
	static entelea_lock_t guard_lock;
	static entelea_wallclock_t guard_wall;
	static entelea_guard_store_t guard_store;
	entelea_result_t r = entelea_platform_clock_init(&guard_clock);

	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_guard_bind_clock(&guard_clock);
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_platform_guard_lock_init(&guard_lock);
	if (r == ENTELEA_OK) {
		r = entelea_guard_bind_lock(&guard_lock);
	}
	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_platform_guard_wallclock_init(&guard_wall);
	if (r == ENTELEA_OK) {
		r = entelea_guard_bind_wallclock(&guard_wall);
	}
	if (r != ENTELEA_OK) {
		return r;
	}
	/* The persistent port lives in flash storage where there is one, and
	 * this step runs before storage_init: mount the backend here first
	 * (mounting is idempotent). A mount failure is not this step's to
	 * report: storage_init fails safe on it. Here it only leaves the
	 * port unbound. */
	(void) mount_flash_backend();
	/* A missing or failed persistent port does not block boot; the gate
	 * fails closed while unbound. */
	r = entelea_platform_guard_store_init(&guard_store);
	if (r == ENTELEA_OK) {
		r = entelea_guard_bind_store(&guard_store);
	}
	if (r != ENTELEA_OK) {
		printk("[guard] store unbound (%d): model calls fail closed\n",
		       r);
	}
	(void) entelea_platform_guard_bootguard();
	return ENTELEA_OK; /* SAFE-HALT left to assembly layer (see above) */
}

static entelea_result_t
step_estop_gpio(void)
{
	return entelea_safety_estop_init();
}

static entelea_result_t
step_safety_poweron(void)
{
	return entelea_safety_poweron_init();
}

static entelea_result_t
step_wdt_start(void)
{
	return entelea_wdt_start();
}

static entelea_result_t
step_supervision_ready(void)
{
	/* Order-anchoring placeholder (same as legacy core_init): events and
	 * clocks are statically bound by the assembly main before the executor
	 * runs; no dynamic init. */
	return ENTELEA_OK;
}

static entelea_result_t
step_storage_init(void)
{
	/* R03 tail append: storage init and noinit consumption. The last
	 * abnormal-reset trace is reported via entelea_storage_last_reset
	 * by the assembly/observation surface (printing stays in the
	 * platform layer). */
	entelea_result_t r = mount_flash_backend();
	unsigned foreign;

	if (r != ENTELEA_OK) {
		return r;
	}
	r = entelea_storage_init();
	if (r != ENTELEA_OK) {
		return r;
	}
	foreign = entelea_storage_audit_foreign_slots();
	if (foreign != 0) {
		/* For instance a board whose storage partition still holds
		 * the previous firmware's data. */
		printk("[storage] audit ring: %u slots hold content that is "
		       "not this ring's records; ignored, never reused\n",
		       foreign);
	}
	return ENTELEA_OK;
}

const entelea_boot_step_t
	entelea_platform_boot_steps[ENTELEA_PLATFORM_BOOT_STEP_COUNT] = {
		{ "guard_bootguard", step_guard_bootguard,
		  ENTELEA_BOOT_STEP_SAFE_STATE },
		{ "estop_gpio", step_estop_gpio, ENTELEA_BOOT_STEP_SAFE_STATE },
		{ "safety_poweron", step_safety_poweron,
		  ENTELEA_BOOT_STEP_SAFE_STATE },
		{ "wdt_start", step_wdt_start, ENTELEA_BOOT_STEP_SAFE_STATE },
		{ "supervision_ready", step_supervision_ready,
		  ENTELEA_BOOT_STEP_SAFE_STATE },
		{ "storage_init", step_storage_init, 0 },
	};
