# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tests_wifi_diagnostic)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr)
option(TEST_CLOUD_DIAGNOSTIC "Exercise the cloud diagnostic assembly" OFF)
if(TEST_CLOUD_DIAGNOSTIC)
  target_compile_definitions(app PRIVATE TEST_CLOUD_DIAGNOSTIC=1)
endif()
target_sources(app PRIVATE src/main.c)
