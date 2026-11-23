/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_BOOT_STEPS_H
#define ENTELEA_PLATFORM_BOOT_STEPS_H

/* R02 platform assembly: supervision boot order table (the order is the
 * specification; only append at the tail, never reorder existing entries;
 * the ztest guard is in platform/zephyr/tests/core).
 * Legacy M1 four-step mapping: estop_gpio/safety_poweron/wdt_start keep their
 * names; the core_init placeholder is renamed supervision_ready (content is
 * still an order-anchoring placeholder: events and clocks are statically
 * bound, no dynamic init; the assembly main completes binding before the
 * executor runs). */

#include <stdbool.h>
#include <stddef.h>
#include <entelea/supervision.h>

#define ENTELEA_PLATFORM_BOOT_STEP_COUNT 6

extern const entelea_boot_step_t
	entelea_platform_boot_steps[ENTELEA_PLATFORM_BOOT_STEP_COUNT];

/* D12 reset-storm SAFE-HALT (strong implementation in guard_bootguard.c;
 * targets that do not link the guard get a weak stub that always returns
 * false). The assembly layer queries it after boot: true = do not start
 * business, keep outputs at the safe initial state, feed the watchdog
 * continuously (exit only via POR). Business board images must link the
 * guard: minimal, hw_cloud_chat and hw_factory_intake do; hw_flash_persist
 * and hw_wifi_netmon currently do not (open item, see
 * docs/rf1-validation.md). */
bool
entelea_platform_guard_bootguard(void);
bool
entelea_platform_guard_safe_halt(void);

#ifdef ENTELEA_TEST
void
entelea_platform_guard_test_reset(void);
#endif

#endif /* ENTELEA_PLATFORM_BOOT_STEPS_H */
