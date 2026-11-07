# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_p4_touch_id)

target_sources(app PRIVATE src/main.c src/probe.c)

if(CONFIG_TS_P4_BOOT_TRACE)
  include(clock_trace.cmake)
  target_sources(app PRIVATE src/boot_trace.c)
  target_link_options(app PUBLIC
    -Wl,--wrap=hardware_init
    -Wl,--wrap=soc_hw_init
    -Wl,--wrap=ana_reset_config
    -Wl,--wrap=super_wdt_auto_feed
    -Wl,--wrap=bootloader_clock_configure
    -Wl,--wrap=rtc_clk_8m_enable
    -Wl,--wrap=esp_rom_delay_us
    -Wl,--wrap=get_act_hp_dbias
    -Wl,--wrap=get_act_lp_dbias
    -Wl,--wrap=regi2c_exit_critical
    -Wl,--wrap=z_riscv_fault
    -Wl,--wrap=z_fatal_error)
endif()
