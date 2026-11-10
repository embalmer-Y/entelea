# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tests_assist)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr)
target_sources(app PRIVATE
  src/main.c
  ${ENTELEA_ROOT}/framework/services/assistant/assistant.c
  ${ENTELEA_ROOT}/framework/services/assistant/model.c
  ${ENTELEA_ROOT}/framework/services/net/netmon.c
  ${ENTELEA_ROOT}/framework/services/config/d12.c
  ${ENTELEA_ROOT}/framework/services/storage/part.c
  ${ENTELEA_ROOT}/framework/services/storage/meta.c
  ${ENTELEA_ROOT}/framework/services/storage/noinit.c
  ${ENTELEA_ROOT}/framework/services/storage/audit.c
  ${ENTELEA_ROOT}/framework/services/storage/trust.c
  ${ENTELEA_ROOT}/framework/services/storage/kv.c
  ${ENTELEA_ROOT}/framework/services/config/prov.c
  ${ENTELEA_ROOT}/framework/services/config/prov_maint.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/wdt.c
  ${ENTELEA_ROOT}/framework/execution/safety/channel.c
  ${ENTELEA_ROOT}/framework/execution/safety/commit.c
  ${ENTELEA_ROOT}/framework/execution/safety/force.c
  ${ENTELEA_ROOT}/framework/execution/safety/drivers.c
  ${ENTELEA_ROOT}/framework/execution/capability/perm.c
  ${ENTELEA_ROOT}/framework/execution/capability/desc.c
  ${ENTELEA_ROOT}/framework/execution/resource/power.c
  ${ENTELEA_ROOT}/framework/execution/task/task.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/wdt.c
  ${ENTELEA_ROOT}/framework/common/time.c
)
target_compile_definitions(app PRIVATE ENTELEA_TEST=1)
