# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_p4_touch)

target_include_directories(app PRIVATE ../p4touchid/src)
target_sources(app PRIVATE src/main.c src/events.c ../p4touchid/src/probe.c)
