# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pdmdemo)

target_compile_definitions(app PRIVATE
  PDMD_WIFI_SSID="${PDMD_WIFI_SSID}"
  PDMD_WIFI_PASSWORD="${PDMD_WIFI_PASSWORD}")

target_sources(app PRIVATE src/main.c)
