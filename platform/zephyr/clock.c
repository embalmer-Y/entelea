/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Platform monotonic clock adapter (R02): wraps Zephyr k_uptime (shared by
 * native_sim and boards) as the R01 instance clock contract. The raw read is
 * a single counter read and is ISR-safe; the wdt feed ISR call path relies
 * on this property (see entelea/wdt.h and the supervision contract).
 * clock_id is reassigned across reboots (per the R01 time contract, old
 * deadlines are not compared across reboots).
 */
#include <zephyr/kernel.h>
#include <entelea/time.h>

#define ENTELEA_PLATFORM_CLOCK_ID 1U

static entelea_result_t
uptime_read_ms(void *user, uint64_t *out)
{
	ARG_UNUSED(user);
	*out = k_uptime_get();
	return ENTELEA_OK;
}

entelea_result_t
entelea_platform_clock_init(entelea_clock_t *clock)
{
	return entelea_clock_init(clock, uptime_read_ms, NULL,
				  ENTELEA_PLATFORM_CLOCK_ID);
}
