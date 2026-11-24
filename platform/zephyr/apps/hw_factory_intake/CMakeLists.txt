# SPDX-License-Identifier: Apache-2.0
# D-3 最小工厂闭环闸：WAMR 编译面同 tests/wasm（fast interp+配额+WASI
# 关+池 256KB）；工厂产物头经 FACTORY_PKG_HEADER 注入（默认=factory/forge
# artifacts/run004，SHA256 固定）。板级 SRAM 预算：ENTELEA_NO_RAM_BACKEND=1（flash
# 后端为唯一持久面——同 hw_flash_persist 纪律）。
# FACTORY_DEV_AUTOCONFIRM=ON：开发自动确认+执行夹具（无用户验收效力）。
cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
# Tracked in factory/forge; only the run004 default is pinned by SHA256 below.
set(FACTORY_PKG_RUN004 ${ENTELEA_ROOT}/factory/forge/artifacts/run004/factory_pkg.h)
set(FACTORY_PKG_RUN004_SHA256 41231020ad06b407707a4fb5bc1f968c81343673d85d4ec10c9c539620e94aed)
set(FACTORY_PKG_HEADER ${FACTORY_PKG_RUN004}
    CACHE STRING "Forge 工厂产物头（tsap_pack.py 生成）")
option(FACTORY_DEV_AUTOCONFIRM "dev-only auto-confirm (no user acceptance)" OFF)
option(FACTORY_TEST_STOP_AFTER_PENDING "native_sim restart test fixture" OFF)
if(FACTORY_TEST_STOP_AFTER_PENDING AND NOT BOARD MATCHES "^native_sim")
  message(FATAL_ERROR "FACTORY_TEST_STOP_AFTER_PENDING is only for native_sim")
endif()
if(NOT EXISTS ${FACTORY_PKG_HEADER})
  message(FATAL_ERROR "factory package header missing: ${FACTORY_PKG_HEADER}")
endif()
get_filename_component(_factory_pkg_abs ${FACTORY_PKG_HEADER} ABSOLUTE)
if(_factory_pkg_abs STREQUAL FACTORY_PKG_RUN004)
  file(SHA256 ${FACTORY_PKG_RUN004} _factory_pkg_sha)
  if(NOT _factory_pkg_sha STREQUAL FACTORY_PKG_RUN004_SHA256)
    message(FATAL_ERROR "run004 factory package header SHA256 mismatch: ${_factory_pkg_sha}")
  endif()
endif()

if(TEST_FLASH_BACKEND)
  # 须在 find_package 前（DT 处理时序）：native_sim sim-flash 变体
  # storage 扩容（同 tests/store 纪律）。
  set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/flash.overlay)
endif()
# RF1 G5：板级业务镜像必须接 D12 复位风暴防护（retained RAM 仅板级有）。
if(BOARD MATCHES "^esp32p4")
  set(ENTELEA_GUARD_BOOTGUARD ON)
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/retained.overlay)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/retained.conf)
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_hw_factory_intake)

set(WAMR_DIR ${ENTELEA_ROOT}/.deps/wamr)
if(NOT EXISTS ${WAMR_DIR}/core/iwasm/include/wasm_export.h)
  message(FATAL_ERROR "pinned WAMR checkout missing at ${WAMR_DIR}")
endif()

set(WAMR_BUILD_PLATFORM "zephyr")
if(CONFIG_64BIT OR BOARD_QUALIFIERS MATCHES "64")
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)")
    set(WAMR_BUILD_TARGET "AARCH64")
  else()
    set(WAMR_BUILD_TARGET "X86_64")
  endif()
  set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 0)
elseif(CONFIG_RISCV)
  # 板级 ESP32-P4 HP（rv32imafc/ABI ilp32f——r03-hwflash build.ninja
  # 实测）：fast interp 平台层最小面（无 AOT/JIT）。
  set(WAMR_BUILD_TARGET "RISCV32_ILP32F")
  set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 1)
else()
  set(WAMR_BUILD_TARGET "X86_32")
  set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 1)
endif()
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_GLOBAL_HEAP_POOL 1)
set(WAMR_BUILD_GLOBAL_HEAP_SIZE 262144)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(WAMR_ROOT_DIR ${WAMR_DIR})
zephyr_include_directories(${ZEPHYR_BINARY_DIR}/include/generated/zephyr)
include(${WAMR_DIR}/build-scripts/runtime_lib.cmake)

get_filename_component(FACTORY_PKG_DIR ${FACTORY_PKG_HEADER} DIRECTORY)
zephyr_include_directories(${WAMR_DIR}/core/iwasm/include
                           ${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/framework/services/package
                           ${ENTELEA_ROOT}/platform/zephyr
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${FACTORY_PKG_DIR})
# The C library's allocator serves from the devicetree pools (the board's PSRAM).
include(${ENTELEA_ROOT}/platform/zephyr/libc_heap.cmake)
entelea_libc_heap_enable()

target_compile_definitions(app PRIVATE ENTELEA_NO_RAM_BACKEND=1)
if(FACTORY_DEV_AUTOCONFIRM)
  target_compile_definitions(app PRIVATE FACTORY_DEV_AUTOCONFIRM=1)
endif()
if(FACTORY_TEST_STOP_AFTER_PENDING)
  target_compile_definitions(app PRIVATE FACTORY_TEST_STOP_AFTER_PENDING=1)
endif()
target_sources(app PRIVATE
  main.c
  ${WAMR_RUNTIME_LIB_SOURCE}
  ${ENTELEA_ROOT}/platform/zephyr/boot_steps.c
  ${ENTELEA_ROOT}/platform/zephyr/guard_ports.c
  ${ENTELEA_ROOT}/platform/zephyr/clock.c
  ${ENTELEA_ROOT}/platform/zephyr/flash_store.c
  ${ENTELEA_ROOT}/framework/common/time.c
  ${ENTELEA_ROOT}/framework/services/storage/part.c
  ${ENTELEA_ROOT}/framework/services/storage/meta.c
  ${ENTELEA_ROOT}/framework/services/storage/noinit.c
  ${ENTELEA_ROOT}/framework/services/storage/audit.c
  ${ENTELEA_ROOT}/framework/services/storage/trust.c
  ${ENTELEA_ROOT}/framework/services/storage/kv.c
  ${ENTELEA_ROOT}/framework/services/config/prov.c
  ${ENTELEA_ROOT}/framework/services/config/prov_maint.c
  ${ENTELEA_ROOT}/framework/services/package/store.c
  ${ENTELEA_ROOT}/framework/services/package/crypto/tweetnacl.c
  ${ENTELEA_ROOT}/framework/services/package/wasm_host.c
  ${ENTELEA_ROOT}/framework/services/config/guard.c
  ${ENTELEA_ROOT}/framework/services/delivery/delivery.c
  ${ENTELEA_ROOT}/framework/execution/capability/perm.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/wdt.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/boot.c
  ${ENTELEA_ROOT}/framework/execution/safety/channel.c
  ${ENTELEA_ROOT}/framework/execution/safety/commit.c
  ${ENTELEA_ROOT}/framework/execution/safety/force.c
  ${ENTELEA_ROOT}/framework/execution/safety/drivers.c
)
if(ENTELEA_GUARD_BOOTGUARD)
  target_compile_definitions(app PRIVATE ENTELEA_REQUIRE_GUARD_BOOTGUARD=1)
  target_sources(app PRIVATE ${ENTELEA_ROOT}/platform/zephyr/guard_bootguard.c)
endif()
# WAMR zephyr 平台层引用旧 __stdout_hook_install（minimal-libc 链接垫片）
if(NOT CONFIG_PICOLIBC)
  target_sources(app PRIVATE src/wamr_compat.c)
endif()
set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES COMPILE_OPTIONS "-w")
if(NOT CONFIG_64BIT)
  set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES
    COMPILE_OPTIONS "-w;-Wa,--noexecstack")
endif()
