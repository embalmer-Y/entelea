# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_camera_contract)

target_sources(app PRIVATE src/main.c ../../module/tessera/src/hal/av.c)
target_compile_definitions(app PRIVATE
  CONFIG_TS_HAL_AV_TIMEOUT_MS=400
  CONFIG_TS_HAL_AV_RECOVER_N=3)
