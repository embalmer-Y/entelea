# SPDX-License-Identifier: Apache-2.0
# R05 Wasm 闭环测试：WAMR（pinned ../entelea-legacy/.deps/wamr）经旧
# DEC-17/25/31/48③ 编译面（fast 解释器+指令配额+WASI 关+池）。
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tests_wasm)

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
# 上游 2.4.5 平台头裸 include <autoconf.h>——生成目录显式前置（旧 §76
# 同纪律，零上游补丁）。
zephyr_include_directories(${ZEPHYR_BINARY_DIR}/include/generated/zephyr)
include(${WAMR_DIR}/build-scripts/runtime_lib.cmake)
list(LENGTH WAMR_RUNTIME_LIB_SOURCE WAMR_SRC_COUNT)
message(STATUS "WAMR_SRC_COUNT=${WAMR_SRC_COUNT}")

# app 模式下独立 zephyr 库不保序（生成头晚于三方编译）且不入链——
# WAMR 源直接编入 app 目标（app 在 zephyr 完整生成后编译，次序正确）。
zephyr_include_directories(${WAMR_DIR}/core/iwasm/include
                           ${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/framework/services/package
                           ${ENTELEA_ROOT}/platform/zephyr)
target_sources(app PRIVATE
  src/main.c
  ${WAMR_RUNTIME_LIB_SOURCE}
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
  ${ENTELEA_ROOT}/framework/services/package/lua_feasibility.c
  ${ENTELEA_ROOT}/framework/services/package/wasm_host.c
  ${ENTELEA_ROOT}/framework/services/config/d12.c
  ${ENTELEA_ROOT}/framework/execution/capability/perm.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/common/time.c
)
target_compile_definitions(app PRIVATE ENTELEA_TEST=1)
# 三方源 -w（钉版上游零补丁纪律）；32 位汇编栈注记。
set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES COMPILE_OPTIONS "-w")
if(NOT CONFIG_64BIT)
  set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES
    COMPILE_OPTIONS "-w;-Wa,--noexecstack")
endif()
# WAMR zephyr 平台层引用旧 __stdout_hook_install（minimal-libc 链接垫片）
if(NOT CONFIG_PICOLIBC)
  target_sources(app PRIVATE src/wamr_compat.c)
endif()
