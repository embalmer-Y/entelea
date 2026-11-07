# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_touch_id_test)

target_include_directories(app PRIVATE ../board/p4touchid/src)
target_sources(app PRIVATE src/main.c ../board/p4touchid/src/probe.c)
