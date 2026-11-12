# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_tests_affect)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr)
option(TEST_AFFECT_COLD "Test production affect initialization" OFF)
if(TEST_AFFECT_COLD)
  target_sources(app PRIVATE src/cold_start.c)
else()
  target_sources(app PRIVATE src/main.c)
  target_compile_definitions(app PRIVATE ENTELEA_TEST=1)
endif()
target_sources(app PRIVATE ${ENTELEA_ROOT}/framework/services/affect/affect.c)
