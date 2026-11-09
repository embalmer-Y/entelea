# SPDX-License-Identifier: Apache-2.0
# D-3 最小工厂闭环闸：WAMR 编译面同 tests/wasm（fast interp+配额+WASI
# 关+池 256KB）；工厂产物头经 FACTORY_PKG_HEADER 注入（默认=forge
# artifacts/run001）。板级 SRAM 预算：ENTELEA_NO_RAM_BACKEND=1（flash
# 后端为唯一持久面——同 hw_flash_persist 纪律）。
cmake_minimum_required(VERSION 3.20.0)

set(FACTORY_PKG_HEADER "/home/emb/project/workspace/forge/artifacts/run003/factory_pkg.h"
    CACHE STRING "Forge 工厂产物头（tsap_pack.py 生成）")
if(NOT EXISTS ${FACTORY_PKG_HEADER})
  message(FATAL_ERROR "factory package header missing: ${FACTORY_PKG_HEADER}")
endif()

if(TEST_FLASH_BACKEND)
  # 须在 find_package 前（DT 处理时序）：native_sim sim-flash 变体
  # storage 扩容（同 tests/store 纪律）。
  set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/flash.overlay)
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_hw_factory_intake)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(WAMR_DIR ${ENTELEA_ROOT}/../entelea-legacy/.deps/wamr)
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
target_compile_definitions(app PRIVATE ENTELEA_NO_RAM_BACKEND=1)
target_sources(app PRIVATE
  main.c
  ${WAMR_RUNTIME_LIB_SOURCE}
  ${ENTELEA_ROOT}/platform/zephyr/boot_steps.c
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
# WAMR zephyr 平台层引用旧 __stdout_hook_install（minimal-libc 链接垫片）
if(NOT CONFIG_PICOLIBC)
  target_sources(app PRIVATE src/wamr_compat.c)
endif()
set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES COMPILE_OPTIONS "-w")
if(NOT CONFIG_64BIT)
  set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES
    COMPILE_OPTIONS "-w;-Wa,--noexecstack")
endif()
