/* SPDX-License-Identifier: Apache-2.0 */
/* R02 platform assembly boot table: estop GPIO -> all-channel safe initial
 * state -> WDT start -> supervision ready (placeholder order anchor). Later
 * business batches only append steps at the tail. */
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain.h> /* __weak */
#include <entelea/guard.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
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

/* Weak board-level flash backend stub (R03 board batch): by default there is
 * no flash surface (RAM backend); targets linking
 * platform/zephyr/flash_store.c get the strong implementation. */
__weak entelea_result_t
entelea_platform_storage_mount_flash(void)
{
	return ENTELEA_E_UNINIT;
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
	 * platform layer).
	 * Board batch: mount the flash backend first. Mount contract
	 * (review P2-2 fix): weak stub E_UNINIT means the target has no
	 * flash surface (legal passthrough to RAM); a strong implementation
	 * returning any other error means **flash configured but unusable**:
	 * the persistence guarantee is void, so the boot step fails safe
	 * (no silent fallback to an idle RAM backend). */
	entelea_result_t m = entelea_platform_storage_mount_flash();

	if (m != ENTELEA_OK && m != ENTELEA_E_UNINIT) {
		return m;
	}
	return entelea_storage_init();
}

const entelea_boot_step_t
	entelea_platform_boot_steps[ENTELEA_PLATFORM_BOOT_STEP_COUNT] = {
		{ "estop_gpio", step_estop_gpio },
		{ "safety_poweron", step_safety_poweron },
		{ "wdt_start", step_wdt_start },
		{ "supervision_ready", step_supervision_ready },
		{ "storage_init", step_storage_init },
		{ "guard_bootguard", step_guard_bootguard },
	};
