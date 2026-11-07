# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_audio_contract)

target_sources(app PRIVATE src/main.c ../../module/tessera/src/hal/av.c)
if(TEST_AUDIO_PDM)
  target_compile_definitions(app PRIVATE CONFIG_TS_DRV_PDM)
elseif(TEST_AUDIO_ES8311)
  target_compile_definitions(app PRIVATE CONFIG_TS_DRV_ES8311)
else()
  message(FATAL_ERROR "Select a test audio backend")
endif()
