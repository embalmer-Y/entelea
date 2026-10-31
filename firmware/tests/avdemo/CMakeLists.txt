# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(avdemo)

target_compile_definitions(app PRIVATE
  AVD_WIFI_SSID="${AVD_WIFI_SSID}"
  AVD_WIFI_PASSWORD="${AVD_WIFI_PASSWORD}")

target_sources(app PRIVATE src/main.c)
