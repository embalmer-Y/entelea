# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_touch_event_test)

target_include_directories(app PRIVATE ../board/p4touch/src)
target_sources(app PRIVATE src/main.c ../board/p4touch/src/events.c)
