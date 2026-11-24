/* SPDX-License-Identifier: Apache-2.0 */
/*
 * For boards with PSRAM only.
 *
 * The in-package PSRAM of the ESP32-P4 and its PLL are fed from the chip's
 * internal LDO channel 2 (1.8 V). ESP-IDF enables that channel from
 * esp_clk_tree_mpll_acquire(), which is its default (ESP_LDO_RESERVE_PSRAM).
 * The Zephyr port compiles the same code without that option and expects a
 * board to declare the channel as a regulator instead. This board declares
 * none, so the driver ran with the PSRAM unpowered and every read returned
 * 0x3b3b3b3b.
 *
 * This powers the channel before the kernel initializes PSRAM, the way
 * ESP-IDF does. It stands in for the board port for as long as the board
 * definition leaves the channel out: enabling Zephyr's regulator driver for
 * it brings in the PMU clock driver, which does not boot on this board.
 */
#include <zephyr/init.h>
#include "esp_rom_sys.h"
#include "hal/ldo_ll.h"

#define PSRAM_LDO_CHANNEL 2
#define PSRAM_LDO_MILLIVOLTS 1800
/* The settle time ESP-IDF waits after switching the channel on. */
#define LDO_STABLE_DELAY_US 200
/* Right before the kernel's own PSRAM initialization, which runs at the
 * default priority. The init macros paste their priority into a section
 * name, so it has to be a plain number. */
#define PSRAM_POWER_PRIORITY 39
BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT == PSRAM_POWER_PRIORITY + 1,
	     "the kernel's PSRAM init moved; keep this one step before it");

static int
psram_power(void)
{
	int unit = LDO_ID2UNIT(PSRAM_LDO_CHANNEL);
	uint8_t dref = 0;
	uint8_t mul = 0;
	bool rail = false;

	ldo_ll_set_owner(unit, LDO_LL_UNIT_OWNER_SW);
	ldo_ll_enable_ripple_suppression(unit, true);
	ldo_ll_voltage_to_dref_mul(unit, PSRAM_LDO_MILLIVOLTS, &dref, &mul,
				   &rail);
	ldo_ll_adjust_voltage(unit, dref, mul, rail);
	ldo_ll_enable(unit, true);
	esp_rom_delay_us(LDO_STABLE_DELAY_US);
	return 0;
}

SYS_INIT(psram_power, PRE_KERNEL_1, PSRAM_POWER_PRIORITY);
