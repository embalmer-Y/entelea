// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <esp_attr.h>
#include <esp_rom_serial_output.h>
#include <esp_rom_sys.h>
#include <soc/rtc.h>
#include <soc/soc.h>
#include <soc/timer_group_reg.h>
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/arch/riscv/exception.h>

#include "boot_trace.h"

static bool clock_active;
static RTC_NOINIT_ATTR uint32_t clock_checkpoint;
static RTC_NOINIT_ATTR uint32_t clock_delay_count;
static RTC_NOINIT_ATTR uint32_t clock_delay_us;
static RTC_NOINIT_ATTR uint32_t clock_fine_checkpoint;
static RTC_NOINIT_ATTR uint32_t clock_calibration_polls;
static RTC_NOINIT_ATTR uint32_t boot_wdt_config0;
static RTC_NOINIT_ATTR uint32_t boot_wdt_config1;
static RTC_NOINIT_ATTR uint32_t boot_wdt_config2;
#ifdef CONFIG_TS_P4_BOOT_SOFT_RESETS
static RTC_NOINIT_ATTR uint32_t soft_reset_state;
#endif

static void IRAM_ATTR checkpoint(uint32_t *target, char stage, char edge)
{
	REG_WRITE((uintptr_t)target, 0xb0040000U | ((uint32_t)(uint8_t)stage << 8) | (uint8_t)edge);
}

void IRAM_ATTR ts_p4_clock_checkpoint(char stage, char edge)
{
	if (clock_active) {
		checkpoint(&clock_fine_checkpoint, stage, edge);
	}
}

void IRAM_ATTR ts_p4_clock_poll(void)
{
	if (clock_active) {
		REG_WRITE((uintptr_t)&clock_calibration_polls,
			  REG_READ((uintptr_t)&clock_calibration_polls) + 1);
	}
}

static void IRAM_ATTR print_clock_state(void)
{
	uint32_t fine = REG_READ((uintptr_t)&clock_fine_checkpoint);

	if ((fine & 0xffff0000U) == 0xb0040000U) {
		esp_rom_printf(DRAM_STR("@FINE=%c%c polls=%u\n"), (int)((fine >> 8) & 0xff),
			       (int)(fine & 0xff), REG_READ((uintptr_t)&clock_calibration_polls));
	} else {
		esp_rom_printf(DRAM_STR("@FINE=unavailable\n"));
	}
	esp_rom_printf(DRAM_STR("@WDT0 at=entry config0=%08x config1=%08x hold=%u\n"),
		       REG_READ((uintptr_t)&boot_wdt_config0),
		       REG_READ((uintptr_t)&boot_wdt_config1),
		       REG_READ((uintptr_t)&boot_wdt_config2));
}

static void IRAM_ATTR trace(char stage, char edge)
{
	/* Flash rodata and Zephyr console are not available at these stages. */
	esp_rom_printf(DRAM_STR("@%c%c\n"), stage, edge);
	esp_rom_output_tx_wait_idle(0);
}

static void IRAM_ATTR trace_clock(char stage, char edge)
{
	if (clock_active) {
		checkpoint(&clock_checkpoint, stage, edge);
	}
#ifdef CONFIG_TS_P4_BOOT_TRACE_DETAILS
	if (clock_active) {
		trace(stage, edge);
	}
#else
	(void)stage;
	(void)edge;
#endif
}

int IRAM_ATTR __wrap_hardware_init(void)
{
	unsigned int reason = esp_rom_get_reset_reason(0);
	int ret;

	trace('H', '+');
	if (reason == RESET_REASON_CPU_MWDT || reason == RESET_REASON_CPU_RWDT ||
	    reason == RESET_REASON_CORE_MWDT || reason == RESET_REASON_CORE_RWDT ||
	    reason == RESET_REASON_SYS_RWDT) {
		uint32_t saved = REG_READ((uintptr_t)&clock_checkpoint);

		if ((saved & 0xffff0000U) == 0xb0040000U) {
			esp_rom_printf(DRAM_STR("@LAST=%c%c delay_index=%u delay_us=%u\n"),
				       (int)((saved >> 8) & 0xff), (int)(saved & 0xff),
				       REG_READ((uintptr_t)&clock_delay_count),
				       REG_READ((uintptr_t)&clock_delay_us));
			print_clock_state();
		} else {
			esp_rom_printf(DRAM_STR("@LAST=unavailable\n"));
		}
		esp_rom_output_tx_wait_idle(0);
	}
	REG_WRITE((uintptr_t)&clock_checkpoint, 0);
	REG_WRITE((uintptr_t)&clock_fine_checkpoint, 0);
	REG_WRITE((uintptr_t)&boot_wdt_config0, REG_READ(TIMG_WDTCONFIG0_REG(0)));
	REG_WRITE((uintptr_t)&boot_wdt_config1, REG_READ(TIMG_WDTCONFIG1_REG(0)));
	REG_WRITE((uintptr_t)&boot_wdt_config2, REG_READ(TIMG_WDTCONFIG2_REG(0)));
	ret = __real_hardware_init();
	trace('H', '-');
	print_clock_state();
	esp_rom_printf(DRAM_STR("@WDT0 at=after config0=%08x config1=%08x hold=%u\n"),
		       REG_READ(TIMG_WDTCONFIG0_REG(0)), REG_READ(TIMG_WDTCONFIG1_REG(0)),
		       REG_READ(TIMG_WDTCONFIG2_REG(0)));
	return ret;
}

void IRAM_ATTR __wrap_soc_hw_init(void)
{
	trace('S', '+');
	__real_soc_hw_init();
	trace('S', '-');
}

void IRAM_ATTR __wrap_ana_reset_config(void)
{
	trace('A', '+');
	__real_ana_reset_config();
	trace('A', '-');
}

void IRAM_ATTR __wrap_super_wdt_auto_feed(void)
{
	trace('W', '+');
	__real_super_wdt_auto_feed();
	trace('W', '-');
}

void IRAM_ATTR __wrap_bootloader_clock_configure(void)
{
	trace('C', '+');
	clock_active = true;
	REG_WRITE((uintptr_t)&clock_delay_count, 0);
	REG_WRITE((uintptr_t)&clock_delay_us, 0);
	REG_WRITE((uintptr_t)&clock_calibration_polls, 0);
	checkpoint(&clock_checkpoint, 'C', '+');
	__real_bootloader_clock_configure();
	checkpoint(&clock_checkpoint, 'C', '-');
	clock_active = false;
	trace('C', '-');
}

void IRAM_ATTR __wrap_rtc_clk_8m_enable(bool enable)
{
	trace_clock('R', '+');
	__real_rtc_clk_8m_enable(enable);
	trace_clock('R', '-');
}

void IRAM_ATTR __wrap_esp_rom_delay_us(uint32_t delay)
{
	if (clock_active) {
		REG_WRITE((uintptr_t)&clock_delay_count,
			  REG_READ((uintptr_t)&clock_delay_count) + 1);
		REG_WRITE((uintptr_t)&clock_delay_us, delay);
	}
	trace_clock('D', '+');
	__real_esp_rom_delay_us(delay);
	trace_clock('D', '-');
}

uint32_t IRAM_ATTR __wrap_get_act_hp_dbias(void)
{
	uint32_t value;

	trace_clock('P', '+');
	value = __real_get_act_hp_dbias();
	trace_clock('P', '-');
	return value;
}

uint32_t IRAM_ATTR __wrap_get_act_lp_dbias(void)
{
	uint32_t value;

	trace_clock('L', '+');
	value = __real_get_act_lp_dbias();
	trace_clock('L', '-');
	return value;
}

void IRAM_ATTR __wrap_regi2c_exit_critical(void)
{
	trace_clock('E', '+');
	__real_regi2c_exit_critical();
	trace_clock('E', '-');
}

void IRAM_ATTR __wrap_z_riscv_fault(struct arch_esf *esf)
{
	esp_rom_printf(DRAM_STR("@FAULT cause=%08lx pc=%08lx value=%08lx\n"),
		       (unsigned long)csr_read(mcause), (unsigned long)csr_read(mepc),
		       (unsigned long)csr_read(mtval));
	if (esf) {
		esp_rom_printf(DRAM_STR("@CONTEXT a0=%08lx ra=%08lx status=%08lx\n"), esf->a0,
			       esf->ra, esf->mstatus);
	} else {
		esp_rom_printf(DRAM_STR("@CONTEXT unavailable\n"));
	}
	esp_rom_printf(DRAM_STR("@CLOCK checkpoint=%08x delay_index=%u delay_us=%u\n"),
		       REG_READ((uintptr_t)&clock_checkpoint),
		       REG_READ((uintptr_t)&clock_delay_count),
		       REG_READ((uintptr_t)&clock_delay_us));
	print_clock_state();
	esp_rom_output_tx_wait_idle(0);
	__real_z_riscv_fault(esf);
}

void IRAM_ATTR __wrap_z_fatal_error(unsigned int reason, const struct arch_esf *esf)
{
	esp_rom_printf(DRAM_STR("@FATAL reason=%u pc=%08lx\n"), reason,
		       (unsigned long)csr_read(mepc));
	esp_rom_output_tx_wait_idle(0);
	__real_z_fatal_error(reason, esf);
}

#ifdef CONFIG_TS_P4_BOOT_SOFT_RESETS
int ts_p4_boot_soft_resets(void)
{
	unsigned int reason = esp_rom_get_reset_reason(0);
	uint32_t state = REG_READ((uintptr_t)&soft_reset_state);

	if (reason == RESET_REASON_CHIP_POWER_ON) {
		state = 0xb0050000U;
	} else if (reason != RESET_REASON_CPU_SW || (state & 0xfffffff8U) != 0xb0050000U ||
		   (state & 7U) == 0 || (state & 7U) > 5) {
		esp_rom_printf(DRAM_STR("P4SOFT ABORT reset=%u state=%08x\n"), reason, state);
		return -EIO;
	}
	if ((state & 7U) == 5) {
		esp_rom_printf(DRAM_STR("P4SOFT DONE count=5\n"));
		return 0;
	}
	REG_WRITE((uintptr_t)&soft_reset_state, state + 1);
	esp_rom_printf(DRAM_STR("P4SOFT REQUEST index=%u\n"), (state & 7U) + 1);
	esp_rom_output_tx_wait_idle(0);
	esp_rom_software_reset_cpu(0);
	esp_rom_printf(DRAM_STR("P4SOFT FAIL reset_returned\n"));
	return -EIO;
}
#endif
