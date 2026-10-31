# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(otabench)

# WiFi 凭证经环境变量注入（sysbuild 不转发任意 cmake 变量到子镜像——
# ENV{} 直读可靠；仓库不含凭证，军规）。
target_compile_definitions(app PRIVATE
  OTAB_WIFI_SSID="$ENV{OTAB_WIFI_SSID}"
  OTAB_WIFI_PASSWORD="$ENV{OTAB_WIFI_PASSWORD}")

target_sources(app PRIVATE src/main.c)
