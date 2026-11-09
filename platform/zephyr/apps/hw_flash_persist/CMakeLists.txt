# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_hw_flash_persist)
# SRAM 预算：本闸面只走 meta/kv/audit/noinit——prov/trust 不链接
#（store 套件面覆盖它们；板级 SRAM 溢出 5312B 实测约束）。

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr)
# 板级 SRAM 预算：RAM 镜像数组不编（flash 后端为唯一持久面）。
target_compile_definitions(app PRIVATE ENTELEA_NO_RAM_BACKEND=1)
target_sources(app PRIVATE
  main.c
  ${ENTELEA_ROOT}/platform/zephyr/boot_steps.c
  ${ENTELEA_ROOT}/platform/zephyr/clock.c
  ${ENTELEA_ROOT}/platform/zephyr/flash_store.c
  ${ENTELEA_ROOT}/framework/common/time.c
  ${ENTELEA_ROOT}/framework/services/storage/part.c
  ${ENTELEA_ROOT}/framework/services/storage/meta.c
  ${ENTELEA_ROOT}/framework/services/storage/noinit.c
  ${ENTELEA_ROOT}/framework/services/storage/audit.c
  ${ENTELEA_ROOT}/framework/services/storage/kv.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/wdt.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/boot.c
  ${ENTELEA_ROOT}/framework/execution/safety/channel.c
  ${ENTELEA_ROOT}/framework/execution/safety/commit.c
  ${ENTELEA_ROOT}/framework/execution/safety/force.c
  ${ENTELEA_ROOT}/framework/execution/safety/drivers.c
)
