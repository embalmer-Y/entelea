/* SPDX-License-Identifier: Apache-2.0 */
/* D12 platform boot guard (owner v1 conservative scheme, section 9). The
 * cross-reset counter lives in LP-domain retained RAM (r16 hardware-verified:
 * __noinit section variables are unreliable on this board's reset path, and
 * the storm counter reset to zero on every boot; see the retained note below).
 * Reset-cause classification via hwinfo: anything other than a pure POR (WDT,
 * software fail-safe, panic, brown-out, pin) counts as abnormal. On an
 * abnormal reset: (1) count the storm; (2) notify guard to open its 60 s
 * business cooldown window. Three or more consecutive abnormal resets (on
 * boards without an RTC the one-hour semantics is honestly downgraded to a
 * consecutive count, cleared only by a pure POR) -> on this boot (the 4th
 * since POR), SAFE-HALT: outputs held at the safe initial state, no business
 * started, watchdog fed continuously, print [guard] RESET-STORM (exit only via
 * POR). The decision is entelea_guard_reset_guard (pure policy).
 * An unreadable or uncleared reset cause, an unavailable retained device, or a
 * failed read or write all fail closed; only a pure POR may accept a readable
 * but uninitialized retained record. Returns true = SAFE-HALT. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/retained_mem.h>
#include <entelea/guard.h>
#include "boot_steps.h"

#define GUARD_MAGIC 0x4431424DU /* "D12M" */

struct guard_rec {
	uint32_t magic;
	uint32_t count;
};

/* LP-domain retained RAM device (app overlay guardretained node; kept across
 * soft/WDT resets, cleared by POR, so the storm window ends naturally). r16
 * hardware evidence: the __noinit macro does not reach the noinit section,
 * dram0.noinit is the flash-loader recovery section, and the storage noinit
 * slot collides with the last_reset trace; retained RAM is the only viable
 * option. */
static const struct device *const retained =
	DEVICE_DT_GET(DT_NODELABEL(guardretained));

static bool safe_halt;

#ifdef ENTELEA_TEST
void
entelea_platform_guard_test_reset(void)
{
	safe_halt = false;
}
#endif

bool
entelea_platform_guard_safe_halt(void)
{
	return safe_halt;
}

static bool
reset_is_abnormal(uint32_t *cause, bool *known)
{
	int rc = hwinfo_get_reset_cause(cause);

	if (rc != 0) {
		printk("[guard] reset cause read failed: %d\n", rc);
		*known = false;
		return true; /* unreadable: conservatively treat as abnormal */
	}
	rc = hwinfo_clear_reset_cause();
	if (rc != 0) {
		printk("[guard] reset cause clear failed: %d\n", rc);
		*known = false;
		return true;
	}
	*known = true;
	return *cause != RESET_POR;
}

bool
entelea_platform_guard_bootguard(void)
{
	struct guard_rec guard = { 0 };
	uint32_t cause = 0;
	bool cause_known;
	bool abnormal = reset_is_abnormal(&cause, &cause_known);
	bool ready = device_is_ready(retained);
	int read_rc = ready ? retained_mem_read(retained, 0, (uint8_t *) &guard,
						sizeof(guard)) :
			      -ENODEV;
	bool history_ok = read_rc == 0 && guard.magic == GUARD_MAGIC;
	bool halt;

	if (!history_ok) {
		guard.count = 0;
	}
	halt = entelea_guard_reset_guard(abnormal, history_ok, &guard.count);
	if (!cause_known || read_rc != 0) {
		if (read_rc != 0) {
			printk("[guard] retained read failed: %d\n", read_rc);
		}
		guard.count = ENTELEA_GUARD_RESET_STORM_MAX;
		halt = true;
	}
	guard.magic = GUARD_MAGIC;
	entelea_guard_boot_note(abnormal);
	if (abnormal) {
		printk("[guard] abnormal reset #%u cause=0x%x history=%s\n",
		       guard.count, cause, history_ok ? "ok" : "lost");
	}
	int write_rc = ready ? retained_mem_write(retained, 0,
						  (const uint8_t *) &guard,
						  sizeof(guard)) :
			       -ENODEV;

	if (write_rc != 0) {
		printk("[guard] retained write failed: %d\n", write_rc);
		halt = true;
	}
	if (halt) {
		safe_halt = true;
		printk("[guard] RESET-STORM SAFE-HALT: >=%u consecutive "
		       "abnormal resets or reset/retained state unknown; "
		       "outputs at safe poweron, business halted "
		       "(power-cycle to exit)\n",
		       ENTELEA_GUARD_RESET_STORM_MAX);
	}
	return safe_halt;
}
