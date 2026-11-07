# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_scan_diagnostic)

set(diagnostic "${CMAKE_CURRENT_SOURCE_DIR}/../board/p4wifibench/src/scan.c")
set_source_files_properties("${diagnostic}" PROPERTIES
  COMPILE_DEFINITIONS "main=c5_scan_diagnostic")
target_sources(app PRIVATE src/main.c "${diagnostic}")
target_link_options(app PUBLIC -Wl,--wrap=ts_wdt_start)
