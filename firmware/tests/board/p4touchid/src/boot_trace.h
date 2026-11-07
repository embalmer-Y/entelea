/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_P4_BOOT_TRACE_H
#define ENTELEA_P4_BOOT_TRACE_H

#include <stdbool.h>
#include <stdint.h>

struct arch_esf;

void ts_p4_clock_checkpoint(char stage, char edge);
void ts_p4_clock_poll(void);
int ts_p4_boot_soft_resets(void);

int __real_hardware_init(void);
void __real_soc_hw_init(void);
void __real_ana_reset_config(void);
void __real_super_wdt_auto_feed(void);
void __real_bootloader_clock_configure(void);
void __real_rtc_clk_8m_enable(bool enable);
void __real_esp_rom_delay_us(uint32_t delay);
uint32_t __real_get_act_hp_dbias(void);
uint32_t __real_get_act_lp_dbias(void);
void __real_regi2c_exit_critical(void);
void __real_z_riscv_fault(struct arch_esf *esf);
void __real_z_fatal_error(unsigned int reason, const struct arch_esf *esf);

#endif
