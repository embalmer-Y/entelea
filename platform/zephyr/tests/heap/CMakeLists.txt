# SPDX-License-Identifier: Apache-2.0
# libc 分配器测试：malloc 族由设备树声明的内存池供数（platform/zephyr/libc_heap.c）。
# 真机上池子是板级设备树声明的整块 PSRAM；仿真器上是 boards/native_sim.overlay
# 声明的两块。断言一律以登记的池子大小为准，同一个测试在两边运行。
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tests_heap)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/platform/zephyr)
target_sources(app PRIVATE src/main.c)
include(${ENTELEA_ROOT}/platform/zephyr/libc_heap.cmake)
entelea_libc_heap_enable()
# 仿真器里设备树的地址不是主机指针：给声明的区域另配主机内存。
if(CONFIG_ARCH_POSIX)
  target_sources(app PRIVATE src/sim_region_source.c)
endif()
target_compile_definitions(app PRIVATE ENTELEA_TEST=1)
