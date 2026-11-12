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
)
target_link_options(app PUBLIC -Wl,--wrap=z_impl_hwinfo_get_reset_cause
  -Wl,--wrap=z_impl_hwinfo_clear_reset_cause
  -Wl,--wrap=z_impl_device_is_ready)
