# SPDX-License-Identifier: Apache-2.0
# r15 MCU 真云闸（A-1）：WiFi→DHCP→板上 TLS1.3 真模型 chat（同源
# platform/linux/https_chat 合同的板上装配面）。凭据=P4C2 串口注入
# （不入镜像不入仓库，用后 zeroize）。
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_hw_cloud_chat)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr
                           ${CMAKE_CURRENT_SOURCE_DIR}) # mbedtls user config 头可达（子构建共享 zephyr_interface include）
target_compile_definitions(app PRIVATE ENTELEA_NO_RAM_BACKEND=1)
target_sources(app PRIVATE
  main.c
  ${ENTELEA_ROOT}/platform/zephyr/boot_steps.c
  ${ENTELEA_ROOT}/framework/services/config/d12.c
  ${ENTELEA_ROOT}/platform/zephyr/clock.c
  ${ENTELEA_ROOT}/platform/zephyr/flash_store.c
  ${ENTELEA_ROOT}/platform/zephyr/ca_roots.c
  ${ENTELEA_ROOT}/platform/zephyr/https_chat.c
  ${ENTELEA_ROOT}/framework/common/time.c
  ${ENTELEA_ROOT}/framework/services/storage/part.c
  ${ENTELEA_ROOT}/framework/services/storage/meta.c
  ${ENTELEA_ROOT}/framework/services/storage/noinit.c
  ${ENTELEA_ROOT}/framework/services/storage/audit.c
  ${ENTELEA_ROOT}/framework/services/config/prov.c
  ${ENTELEA_ROOT}/framework/services/net/netmon.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/wdt.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/boot.c
  ${ENTELEA_ROOT}/framework/execution/safety/channel.c
  ${ENTELEA_ROOT}/framework/execution/safety/commit.c
  ${ENTELEA_ROOT}/framework/execution/safety/force.c
  ${ENTELEA_ROOT}/framework/execution/safety/drivers.c
)
