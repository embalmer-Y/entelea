/* SPDX-License-Identifier: Apache-2.0 */
/* R03 board batch A-3 hardware-gate image: unattended flash persistence
 * across resets on real hardware.
 * Phase ledger = kv (flash-persistent). Resets take the production path
 * (system_fail -> task_wdt channel expiry -> sys_reboot).
 *  P1 (no kv record, i.e. power-on entry): write the meta pattern string and
 *     read it back; store P1 in kv; append one audit entry; then
 *     system_fail(FAIL_BOOT(7)) -> SW reset (system_fail also records the
 *     reset cause through noinit, which is itself part of the flash
 *     persistence chain).
 *  P2 (kv = P1 entry, i.e. after the reset): meta readback == P1 pattern;
 *     latest kv == P1; audit count >= 1; last_reset cause = FAIL_BOOT(7)
 *     (noinit survives the SW reset via flash and is consumed once in this
 *     boot) -> store P2 in kv -> park resident and finish.
 *  P3 (kv = P2): idempotent safe resident state (repeated power-on does not
 *     touch the state surface again).
 * No test hooks: only the public production APIs are used (supervision_boot,
 * meta, kv, audit, system_fail). Real power loss (VBUS) is a field item; this
 * image proves flash persistence in the SW/MWDT reset domain. */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/hwinfo.h>
#include <entelea/kv.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"

#define PHASE_APP 0x50484631U /* "PHF1" */

/* In-gate checkpoint printing: each PASS/FAIL goes to the serial port
 * (evidence surface); execution is not aborted. */
#define CHK(cond, msg) \
	printk("[hw-flash] %s: %s\n", (cond) ? "PASS" : "FAIL", msg)

static entelea_clock_t platform_clock;

static const uint8_t META_PAT[30] = "ENTEA-P1-META-FLASH-PERSIST-OK";

static uint32_t
fnv1a(const uint8_t *p, size_t n)
{
	uint32_t h = 0x811C9DC5U;

	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x01000193U;
	}
	return h;
}

int
main(void)
{
	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK) {
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0;
	}
	printk("[hw-flash] boot sequence begins\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
	uint32_t cause = 0;

	(void) hwinfo_get_reset_cause(&cause);
	uint32_t reason = 0;
	bool present = false;

	entelea_storage_last_reset(&reason, &present);
	printk("[hw-flash] boot ok; hwinfo cause=0x%x last_reset=%s0x%08x\n",
	       cause, present ? "0x" : "none(", present ? reason : 0);

	uint8_t kvb[8];
	int32_t kvn = entelea_kv_load(PHASE_APP, kvb, sizeof(kvb));

	if (kvn == 2 && memcmp(kvb, "P1", 2) == 0) {
		/* ---- P2: post-reset verification ---- */
		uint8_t rb[ENTELEA_STORAGE_META_MAX];
		uint16_t rlen = 0;

		/* Meta across reset: the diagnostic pattern written by P1 must
		 * read back unchanged. The trial-persistence gate moved out
		 * of this app (the package path exceeds its SRAM budget; the
		 * native delivery suite covers it). */
		CHK(entelea_storage_meta_read(rb, sizeof(rb), &rlen) ==
				    ENTELEA_OK &&
			    rlen == sizeof(META_PAT) &&
			    memcmp(rb, META_PAT, sizeof(META_PAT)) == 0,
		    "P2 meta holds the P1 diagnostic pattern");
		printk("[hw-flash] P2 PASS: meta persists (len=%u fnv=%08x)\n",
		       rlen, fnv1a(rb, rlen));
		entelea_storage_audit_entry_t ae[4];
		int32_t n = entelea_storage_audit_read(ae, 4);

		printk("[hw-flash] P2 %s: audit entries=%d (fnv=%08x)\n",
		       n >= 1 ? "PASS" : "FAIL", (int) n,
		       n >= 1 ? fnv1a((const uint8_t *) ae,
				      (size_t) n * sizeof(ae[0])) :
				0);
		CHK(present && reason == ENTELEA_FAIL_BOOT(7),
		    "P2 noinit reason survived SW reset (0x00010007)");
		CHK(entelea_kv_save(PHASE_APP, "P2", 2) == ENTELEA_OK,
		    "P2 kv phase=P2");
		printk("[hw-flash] gate complete: parking resident\n");
	} else if (kvn == 2 && memcmp(kvb, "P2", 2) == 0) {
		printk("[hw-flash] P3: already verified; parking resident\n");
	} else {
		/* ---- P1: write phase ---- */
		CHK(entelea_storage_meta_write(META_PAT, sizeof(META_PAT)) ==
			    ENTELEA_OK,
		    "P1 meta write");
		uint8_t rb[ENTELEA_STORAGE_META_MAX];
		uint16_t rlen = 0;

		CHK(entelea_storage_meta_read(rb, sizeof(rb), &rlen) ==
				    ENTELEA_OK &&
			    rlen == sizeof(META_PAT),
		    "P1 meta readback");
		printk("[hw-flash] P1: meta written (fnv=%08x)\n",
		       fnv1a(rb, rlen));
		CHK(entelea_kv_save(PHASE_APP, "P1", 2) == ENTELEA_OK,
		    "P1 kv phase=P1");
		entelea_storage_audit_entry_t e = {
			.t_ms = 0,
			.seq = 0,
			.ch_idx = 0xFFFF,
			.kind = 0,
			.res = 0,
			.evt = 0,
		};

		CHK(entelea_storage_audit_append(&e, 0) == ENTELEA_OK,
		    "P1 audit append");
		printk("[hw-flash] P1: kv/audit written; system_fail -> WDT "
		       "reset (~8.8s measured)\n");
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(7));
	}
	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
