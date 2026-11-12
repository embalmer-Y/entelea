/* SPDX-License-Identifier: Apache-2.0 */
/* R07 Linux minimal host: host-surface verification of the same-source
 * contract matrix (D02: minimal native host; the common contract, time
 * adapter, and safety mechanisms (simulated backend) build and run on Linux.
 * The Zephyr kernel surface is not ported and no full Linux companion is
 * included). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/time.h>
#include "clock.h"

static int failures;

#define CHECK(expr)                                                           \
	do {                                                                  \
		if (!(expr)) {                                                \
			printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); \
			failures++;                                           \
		}                                                             \
	} while (0)

static int order_n;
static void
sub_a(const entelea_event_t *e, void *u)
{
	(void) e;
	(void) u;
	order_n++;
}
static void
sub_b(const entelea_event_t *e, void *u)
{
	(void) e;
	(void) u;
	order_n++;
}

int
main(void)
{
	/* 1. Instance clock (Linux adapter). */
	entelea_clock_t clk;

	CHECK(entelea_platform_clock_init(&clk) == ENTELEA_OK);
	entelea_instant_t now;

	CHECK(entelea_clock_now(&clk, &now) == ENTELEA_OK);
	CHECK(now.valid);
	CHECK(now.clock_id == 2);
	uint64_t first = now.ms;

	CHECK(entelea_clock_now(&clk, &now) == ENTELEA_OK);
	CHECK(now.ms >= first); /* Monotonic (CLOCK_MONOTONIC) */

	/* 2. Event bus (static order/capacity). */
	CHECK(entelea_evt_bind_clock(&clk) == ENTELEA_OK);
	CHECK(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_a, NULL) ==
	      ENTELEA_OK);
	CHECK(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_b, NULL) ==
	      ENTELEA_OK);
	order_n = 0;
	const entelea_event_t e = { .id = ENTELEA_EVT_BOOT_STEP };

	entelea_evt_publish(&e);
	CHECK(order_n == 2); /* Synchronous dispatch */

	/* 3. Safety mechanisms (simulated backend): registration, three-state
	 * machine, sole write path, and readback. */
	CHECK(entelea_safety_bind_clock(&clk) == ENTELEA_OK);
	static const entelea_out_channel_t ch = {
		.uid = "led",
		.kind = ENTELEA_CH_GPIO,
		.poweron = { .b = false },
		.linkloss = { .b = false },
		.fault = { .b = false },
	};
	CHECK(entelea_safety_register_channel(&ch) == ENTELEA_OK);
	CHECK(entelea_safety_commit("led",
				    (entelea_out_value_t) { .b = true }) ==
	      ENTELEA_E_STATE); /* Refused in SAFE_POWERON */
	CHECK(entelea_safety_set_link(true) == ENTELEA_OK);
	CHECK(entelea_safety_commit("led", (entelea_out_value_t) {
						   .b = true }) == ENTELEA_OK);
	entelea_out_value_t rb;

	CHECK(entelea_safety_readback("led", &rb) == ENTELEA_OK);
	CHECK(rb.b);
	entelea_safety_force_all_fault();
	entelea_ch_state_t st;

	CHECK(entelea_safety_channel_state("led", &st) == ENTELEA_OK);
	CHECK(st == ENTELEA_ST_SAFE_FAULT);
	CHECK(entelea_safety_commit("led",
				    (entelea_out_value_t) { .b = false }) ==
	      ENTELEA_E_STATE); /* Refused while latched */

	/* 4. Fail reason trace hook (weak stub path; the Linux host has no
	 * storage). */
	entelea_safety_system_fail(ENTELEA_FAIL_BOOT(1));
	CHECK(1); /* Test builds return instead of halting */

	printf(failures == 0 ? "HOST SUITE PASS (linux minimal host)\n" :
			       "HOST SUITE FAIL (%d)\n",
	       failures);
	return failures == 0 ? 0 : 1;
}
