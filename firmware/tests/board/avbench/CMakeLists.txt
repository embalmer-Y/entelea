# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(avbench)

target_compile_definitions(app PRIVATE
  AVB_WIFI_SSID="${AVB_WIFI_SSID}"
  AVB_WIFI_PASSWORD="${AVB_WIFI_PASSWORD}"
  AVB_ROUTER="${AVB_ROUTER}")

target_sources(app PRIVATE src/main.c)
