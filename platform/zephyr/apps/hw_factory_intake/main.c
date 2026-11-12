/* SPDX-License-Identifier: Apache-2.0 */
/* D-3 minimal factory closed-loop hardware gate (D batch; RF1 #9 revision):
 * Forge (deterministic wasm_gen assembly -> tsap_pack.py test root signature)
 * -> device (stage -> verify -> digest cross-check -> factory_intake ->
 * PENDING -> an unconfirmed load is refused by the trial gate). Artifact
 * bytes are injected at build time (factory_pkg.h; the USB flashing delivery
 * path; network delivery follows in batch F).
 * The default build has no user confirmation UI (D04/D09/D10 open): it does
 * not confirm on the user's behalf, and the confirmation window ends
 * fail-closed on timeout (TIMEOUT is persisted; the candidate slot stays
 * refused).
 * FACTORY_DEV_AUTOCONFIRM=ON (development only): auto-confirms the shown
 * object, then runs the run/spin/hang fixtures. This is engineering evidence
 * with no product user-acceptance effect.
 * Any failing step -> system_fail. The trial's cross-reset persistence is
 * proven by hw_flash_persist A-4 and is not repeated here. */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <entelea/appstore.h>
#include <entelea/delivery.h>
#include <entelea/package.h>
#include <entelea/prov.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wasm_host.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"
#include "factory_pkg.h"

#define CHK(cond, msg)                                                    \
	do {                                                              \
		bool ok_ = (cond);                                        \
		printk("[hw-fact] %s: %s\n", ok_ ? "PASS" : "FAIL", msg); \
		if (!ok_) {                                               \
			goto fail;                                        \
		}                                                         \
	} while (0)

static entelea_clock_t platform_clock;

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
	printk("[hw-fact] factory loop gate begins (tsap %uB)\n",
	       (uint32_t) FACTORY_TSAP_LEN);
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
	if (entelea_platform_guard_safe_halt()) {
		/* RF1 G5: reset-storm SAFE-HALT; business is not started,
		 * outputs stay at the safe initial state, and WDT patrol keeps
		 * feeding (exit via POR). */
		printk("[hw-fact] [guard] SAFE-HALT: business disabled\n");
		k_sleep(K_FOREVER);
		return 0;
	}

	bool ok = true;
	entelea_trial_candidate_t prior;

	CHK(entelea_trial_restore() == ENTELEA_OK, "restore trial state");
	entelea_trial_current(&prior);
	CHK(entelea_trial_state() == ENTELEA_TRIAL_NONE ||
		    ((entelea_trial_state() == ENTELEA_TRIAL_TIMEOUT ||
		      entelea_trial_state() == ENTELEA_TRIAL_REJECTED) &&
		     prior.slot == 0),
	    "slot not occupied by a pending or active candidate");

	/* 0) Factory provisioning: inject the factory root pk0 over the
	 *    maintenance channel (prov CBOR v1, minimal 9 pairs). On a real
	 *    line this happens in a flashing window; the gate uses the same
	 *    API: validate first, then one window per boot. pk1 = zero
	 *    (unforgeable). */
	static const uint8_t FACTORY_ROOT_PUB[32] = {
		0xf0, 0x84, 0xa4, 0xe5, 0x6f, 0x68, 0xb6, 0xab,
		0x5c, 0xbe, 0xe8, 0x60, 0x3c, 0x59, 0xa4, 0x2a,
		0xc2, 0xef, 0x26, 0x51, 0xc4, 0x93, 0x94, 0xa7,
		0x03, 0x5c, 0xd5, 0xdb, 0x52, 0xdb, 0x47, 0x7a,
	};
	{
		static uint8_t pcb[192];
		uint32_t n = 0;

		pcb[n++] = 0xA9; /* map(9): minimal prov CBOR v1 set */
#define PUT(lit)                                               \
	do {                                                   \
		if (n + sizeof(lit) - 1 <= sizeof(pcb)) {      \
			memcpy(pcb + n, lit, sizeof(lit) - 1); \
		}                                              \
		n += sizeof(lit) - 1;                          \
	} while (0) /* overflow: skip copy, still count n; guard rejects */
		PUT("\x61"
		    "v");
		pcb[n++] = 0x01; /* v:1 */
		PUT("\x67"
		    "node_id");
		PUT("\x65"
		    "node1");
		PUT("\x67"
		    "cube_id");
		PUT("\x65"
		    "cube1");
		PUT("\x67"
		    "routers");
		PUT("\x81");
		PUT("\x6a"
		    "tcp/f:7447");
		PUT("\x63"
		    "pk0");
		pcb[n++] = 0x58;
		pcb[n++] = 32;
		memcpy(pcb + n, FACTORY_ROOT_PUB, 32);
		n += 32;
		PUT("\x63"
		    "pk1");
		pcb[n++] = 0x58;
		pcb[n++] = 32;
		n += 32; /* pcb is zeroed: pk1 = zero root (unforgeable) */
		PUT("\x64"
		    "cred");
		PUT("\x62"
		    "ok");
		PUT("\x66"
		    "pwr_ma");
		PUT("\x19\x01\xf4"); /* 500mA */
		PUT("\x65"
		    "estop");
		pcb[n++] = 0x01;
#undef PUT
		CHK(n <= sizeof(pcb) && /* assembly guard (review P3-3) */
			    entelea_prov_write_maint(pcb, n) == ENTELEA_OK &&
			    entelea_prov_load() == ENTELEA_OK &&
			    memcmp(entelea_prov()->root_pubkeys[0],
				   FACTORY_ROOT_PUB, 32) == 0,
		    "factory provisioning pk0 (maint channel)");
	}

	/* 1) Stage the factory package in steps (production staging API). */
	ok &= entelea_pkg_stage_begin(0) == ENTELEA_OK;
	uint32_t off = 0;

	while (ok && off < FACTORY_TSAP_LEN) {
		uint32_t n = FACTORY_TSAP_LEN - off;

		if (n > 512) {
			n = 512;
		}
		if (entelea_pkg_stage_chunk(0, off, FACTORY_TSAP + off, n) !=
		    ENTELEA_OK) {
			ok = false;
			break;
		}
		off += n;
	}
	CHK(ok, "stage factory package (begin+chunks)");

	/* 2) Verify the signature against the explicit factory root (same
	 *    source as prov pk0). It is stricter than the default trust set:
	 *    the device checks that the package is signed by the factory root
	 *    declared by Forge. */
	entelea_tsap_view_t v;

	CHK(entelea_pkg_verify(0, FACTORY_ROOT_PUB, &v, NULL) == ENTELEA_OK &&
		    v.wasm_len > 0 && v.manifest_len > 0,
	    "verify signature (explicit factory root)");

	/* 3) Forge/device byte cross-check (D-4 device side): slot content
	 *    sha256 == Forge candidate.package_digest (injected at build). */
	{
		static uint8_t rd[2048];
		uint32_t st[8];
		uint8_t dg[32];

		entelea_sha256_stream_init(st);
		uint32_t got = 0;

		while (got < FACTORY_TSAP_LEN) {
			uint32_t n = FACTORY_TSAP_LEN - got;

			if (n > sizeof(rd)) {
				n = sizeof(rd);
			}
			if (entelea_pkg_slot_read(0, got, rd, n) !=
			    ENTELEA_OK) {
				ok = false;
				break;
			}
			entelea_sha256_stream_update(st, rd, n);
			got += n;
		}
		entelea_sha256_stream_final(st, dg);
		if (memcmp(dg, FACTORY_DIGEST, 32) != 0) {
			printk("[hw-fact] dev=%02x%02x%02x%02x "
			       "forge=%02x%02x%02x%02x\n",
			       dg[0], dg[1], dg[2], dg[3], FACTORY_DIGEST[0],
			       FACTORY_DIGEST[1], FACTORY_DIGEST[2],
			       FACTORY_DIGEST[3]);
		}
		CHK(ok && memcmp(dg, FACTORY_DIGEST, 32) == 0,
		    "slot bytes == forge digest (D-4 device side)");
		/* Gate digest span (canonical length up to end of signature)
		 * must equal Forge package_digest. */
		CHK(v.pkg_len == FACTORY_TSAP_LEN &&
			    entelea_pkg_file_digest(0, v.pkg_len, dg) ==
				    ENTELEA_OK &&
			    memcmp(dg, FACTORY_DIGEST, 32) == 0,
		    "gate digest domain == forge package_digest");
	}

	/* 4) Factory intake -> PENDING_CONFIRM (metadata injected at build
	 *    from the Forge-side declaration; digest, permissions, version
	 *    are the objects the user confirms). */
	{
		entelea_factory_candidate_t fc;

		memset(&fc, 0, sizeof(fc));
		memcpy(fc.package_digest, FACTORY_DIGEST, 32);
		/* The Forge bitmap FACTORY_PERMISSIONS has no mapping to the
		 * permission list yet (owner decision pending). This
		 * diagnostic submits an empty list (no natives grant);
		 * compute-only artifacts are unaffected. */
		fc.permissions_len = 0;
		fc.runtime_version = FACTORY_VERSION;
		fc.payload_kind = FACTORY_KIND;
		fc.budget_steps = FACTORY_BUDGET;
		fc.mem_quota_kb = FACTORY_MEM_KB;
		strncpy(fc.description, FACTORY_DESC,
			sizeof(fc.description) - 1);
		CHK(entelea_factory_intake(&fc, 0) == ENTELEA_OK &&
			    entelea_trial_state() ==
				    ENTELEA_TRIAL_PENDING_CONFIRM &&
			    entelea_trial_flush() == ENTELEA_OK,
		    "factory intake -> PENDING_CONFIRM persisted (slot 0)");
	}

#ifdef FACTORY_TEST_STOP_AFTER_PENDING
	printk("[hw-fact] TEST: stopping after persisted PENDING\n");
	return 0;
#endif

	/* 5) Not executable before confirmation: the trial gate refuses
	 *    the load with no side effects. owner unbound (no imported
	 *    modules; natives do not fire). */
	entelea_principal_t ctx = { 0 };
	uint32_t app = 0;
	entelea_result_t wl = entelea_wasm_init() == ENTELEA_OK ?
				      entelea_wasm_load(0, ctx, &app) :
				      ENTELEA_E_IO;

	printk("[hw-fact] pre-confirm wasm load rc=%d\n", (int) wl);
	CHK(wl == ENTELEA_E_PERM && app == 0 &&
		    entelea_wasm_instance_count() == 0,
	    "load before user confirm -> E_PERM (trial gate)");

#ifndef FACTORY_DEV_AUTOCONFIRM
	/* 6) Product path: no user confirmation; the confirmation window
	 *    times out fail-closed (the gate never confirms for the user). */
	CHK(entelea_trial_timeout() == ENTELEA_OK &&
		    entelea_trial_flush() == ENTELEA_OK &&
		    entelea_wasm_load(0, ctx, &app) == ENTELEA_E_PERM,
	    "no confirm -> TIMEOUT persisted, slot stays gated");
	printk("[hw-fact] PASS: confirmation timed out; candidate remains "
	       "gated "
	       "(no auto-confirm in product build)\n");
	return 0;
#else
	/* 6) Dev auto-confirm: the confirmed object is the shown candidate
	 *    (trial_current). */
	{
		entelea_trial_candidate_t shown;

		entelea_trial_current(&shown);
		printk("[hw-fact] DEV AUTO-CONFIRM (无产品用户验收效力)\n");
		CHK(entelea_trial_confirm(&shown) == ENTELEA_OK &&
			    entelea_trial_state() == ENTELEA_TRIAL_ACTIVE,
		    "dev confirm shown candidate -> ACTIVE");
	}
	wl = entelea_wasm_load(0, ctx, &app);
	printk("[hw-fact] wasm load rc=%d app=%u\n", (int) wl, app);
	CHK(wl == ENTELEA_OK && app == 1, "wasm load (confirmed bytes, app=1)");
	int32_t ret = -1;

	CHK(entelea_wasm_call(app, ctx, "run", 0, 2000, &ret) == ENTELEA_OK &&
		    ret == 1,
	    "call run -> 2*0+1 == 1 (factory artifact executes)");
	ret = -1;
	{
		/* spin(3): returns 0 after three idle iterations (the run003
		 * spin(N>=1) infinite-loop defect surfaces here as E_TIMEOUT;
		 * RF1 #10). */
		entelea_result_t sr =
			entelea_wasm_call(app, ctx, "spin", 3, 2000, &ret);

		printk("[hw-fact] spin(3) rc=%d ret=%d\n", (int) sr, ret);
		CHK(sr == ENTELEA_OK && ret == 0, "call spin(3) -> 0");
	}

	/* 7) Bounded stop of a real infinite loop (R-V1 fixture: hang loops
	 *    unconditionally; step budget expiry -> E_TIMEOUT and instance
	 *    destruction, replacing the wasm test "budget 0" trick as
	 *    evidence). */
	CHK(entelea_wasm_call(app, ctx, "hang", 0, 50, &ret) ==
			    ENTELEA_E_TIMEOUT &&
		    entelea_wasm_instance_count() == 0,
	    "call hang (real infinite loop) -> E_TIMEOUT bounded stop");

	/* 8) Wrap up: end trial, clear slot, flush (safe steady state). */
	CHK(entelea_trial_end() == ENTELEA_OK &&
		    entelea_trial_state() == ENTELEA_TRIAL_NONE &&
		    entelea_trial_flush() == ENTELEA_OK,
	    "trial end + flush (safe steady state)");
	printk("[hw-fact] PASS: factory loop closed (DEV auto-confirm; "
	       "not user acceptance)\n");
	return 0;
#endif

fail:
	entelea_safety_system_fail(ENTELEA_FAIL_BOOT(7));
	return 0;
}
