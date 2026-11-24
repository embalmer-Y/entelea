# SPDX-License-Identifier: Apache-2.0
# r16 D12 运行时闸 ztest（冷却/频度/风暴/fail-closed——owner v1 阈值）。
cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/app.overlay)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tguard)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr)
target_compile_definitions(app PRIVATE ENTELEA_TEST=1)
target_sources(app PRIVATE
  src/main.c
  src/bootguard.c
  ${ENTELEA_ROOT}/framework/services/config/guard.c
  ${ENTELEA_ROOT}/framework/common/time.c
  ${ENTELEA_ROOT}/platform/zephyr/guard_bootguard.c
  ${ENTELEA_ROOT}/platform/zephyr/guard_ports.c
  # Production daily-cap persistence: the KV adapter over the storage port.
  ${ENTELEA_ROOT}/platform/zephyr/guard_store.c
  ${ENTELEA_ROOT}/framework/services/storage/kv.c
  ${ENTELEA_ROOT}/framework/services/storage/part.c
  # 启动表端到端：真实守卫排在首步，业务步骤持续失败时的复位风暴。
  ${ENTELEA_ROOT}/platform/zephyr/boot_steps.c
  ${ENTELEA_ROOT}/platform/zephyr/clock.c
  ${ENTELEA_ROOT}/framework/services/storage/noinit.c
  ${ENTELEA_ROOT}/framework/services/storage/audit.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/wdt.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/boot.c
  ${ENTELEA_ROOT}/framework/execution/safety/channel.c
  ${ENTELEA_ROOT}/framework/execution/safety/commit.c
  ${ENTELEA_ROOT}/framework/execution/safety/force.c
  ${ENTELEA_ROOT}/framework/execution/safety/drivers.c
)
# printk：用例捕获控制台，断言执行器报告了失败与未运行的步骤。
target_link_options(app PUBLIC -Wl,--wrap=z_impl_hwinfo_get_reset_cause
  -Wl,--wrap=z_impl_hwinfo_clear_reset_cause
  -Wl,--wrap=z_impl_device_is_ready -Wl,--wrap=printk)
