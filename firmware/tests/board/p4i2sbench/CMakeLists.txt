# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_p4_i2s_diagnostic)

option(P04_I2S_WORD32 "Use 32-bit stereo slots" OFF)
option(P04_I2S_GDMA_CANDIDATE "Use isolated archived GDMA candidate" OFF)
option(P04_I2S_LIFECYCLE_TEST "Exercise normal STOP and restart" OFF)
if(P04_I2S_LIFECYCLE_TEST)
  if(NOT P04_I2S_WORD32)
    message(FATAL_ERROR "The P4 I2S lifecycle diagnostic requires 32-bit slots")
  endif()
  target_sources(app PRIVATE src/lifecycle.c)
else()
  target_sources(app PRIVATE src/main.c)
endif()

if(P04_I2S_WORD32)
  target_compile_definitions(app PRIVATE P04_I2S_WORD32)
endif()

if(P04_I2S_GDMA_CANDIDATE)
  include(gdma_candidate.cmake)
endif()
