# SPDX-License-Identifier: Apache-2.0
# D03 Lua feasibility on the real, pinned Lua interpreter (.deps/lua).
# Opt-in experiment, off by default:
#   --cmake-arg=-DLUA_VM_FEASIBILITY=ON
# It is not a Lua runtime and is never linked into a product image.
if(NOT LUA_VM_FEASIBILITY)
  message(FATAL_ERROR
    "lua_vm is an opt-in experiment: pass --cmake-arg=-DLUA_VM_FEASIBILITY=ON")
endif()
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tests_lua_vm)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(LUA_DIR ${ENTELEA_ROOT}/.deps/lua)

# The interpreter must be the pinned, unmodified checkout: the repository's
# read-only checker verifies it before anything is compiled.
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${ENTELEA_ROOT}/tools/check_dependencies.py
          --dependency lua --checkout ${LUA_DIR}
  RESULT_VARIABLE lua_pinned
  OUTPUT_VARIABLE lua_out
  ERROR_VARIABLE lua_out
)
if(NOT lua_pinned EQUAL 0)
  message(FATAL_ERROR "pinned Lua checkout missing or modified: ${lua_out}")
endif()

# Core, auxiliary library and every standard library. Excluded on purpose:
# linit (the open-everything entry; the harness opens the libraries itself)
# plus the standalone interpreters lua.c and onelua.c. ldblib is compiled for
# the hazard demonstration only; the harness never opens it for a sandbox.
set(LUA_PARTS
  lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
  lparser lstate lstring ltable ltm lundump lvm lzio lauxlib
  lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib ldblib
  liolib loslib loadlib)
set(LUA_SOURCES "")
foreach(part IN LISTS LUA_PARTS)
  list(APPEND LUA_SOURCES ${LUA_DIR}/${part}.c)
endforeach()

# Stock Lua allows 200 nested C calls and syntax levels (LUAI_MAXCCALLS). It
# is a compile-time choice of the interpreter, so a lower limit is measured
# with it, not by editing the pinned sources.
set(LUA_VM_MAXCCALLS 200 CACHE STRING "LUAI_MAXCCALLS of the pinned Lua (stock: 200)")
# The interpreter runs on a dedicated kernel heap of LUA_VM_HEAP_KB, and the
# harness checks a memory quota of LUA_VM_QUOTA_KB against it (never larger).
# The harness runs on its own thread with a stack of LUA_VM_STACK_KB: Lua's
# parser and C-level nesting use the C stack, and the stock nesting limit of
# 200 needs about 294 KB of it on the ESP32-P4. With CONFIG_ESP_SPIRAM=y the
# heap buffer and that stack are placed in PSRAM.
set(LUA_VM_HEAP_KB 320 CACHE STRING "kernel heap under the interpreter, in KB")
set(LUA_VM_QUOTA_KB 256 CACHE STRING "memory quota under test, in KB")
set(LUA_VM_STACK_KB 1024 CACHE STRING "stack of the harness thread, in KB")

zephyr_include_directories(${ENTELEA_ROOT}/framework/include ${LUA_DIR})
zephyr_library_named(entelea_lua)
zephyr_library_sources(${LUA_SOURCES})
# Upstream Lua keeps its own formatting and warning baseline.
set_source_files_properties(${LUA_SOURCES} PROPERTIES COMPILE_OPTIONS "-w"
  COMPILE_DEFINITIONS LUAI_MAXCCALLS=${LUA_VM_MAXCCALLS})

target_sources(app PRIVATE
  src/main.c
  src/feasibility.c
  # The contract's memory and step gates the harness attaches to the VM,
  # and the real principal and event modules behind the host call.
  ${ENTELEA_ROOT}/framework/services/package/lua_feasibility.c
  ${ENTELEA_ROOT}/framework/execution/capability/perm.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
)
# A board image has no filesystem nor process clock, which stock Lua
# references; the simulator image links the host libc instead.
if(NOT CONFIG_BOARD_NATIVE_SIM)
  target_sources(app PRIVATE src/no_fs_stubs.c)
endif()
# The board definition does not power the PSRAM; see src/psram_power.c.
if(CONFIG_ESP_SPIRAM)
  target_sources(app PRIVATE src/psram_power.c)
endif()
target_link_libraries(app PRIVATE entelea_lua)
target_compile_definitions(app PRIVATE ENTELEA_TEST=1
  LUA_VM_MAXCCALLS=${LUA_VM_MAXCCALLS} LUA_VM_HEAP_KB=${LUA_VM_HEAP_KB}
  LUA_VM_QUOTA_KB=${LUA_VM_QUOTA_KB} LUA_VM_STACK_KB=${LUA_VM_STACK_KB})
