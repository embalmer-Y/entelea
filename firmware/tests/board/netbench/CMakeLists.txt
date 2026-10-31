# SPDX-License-Identifier: Apache-2.0
# netbench（板级四单元）：WiFi + zenoh 命令往返时延基准（xiao_esp32s3）。
# 凭证与路由地址经 cmake 变量注入（本地脚本传入，不入仓库）：
#   west build ... -- -DNETBENCH_WIFI_SSID=... -DNETBENCH_WIFI_PASSWORD=... \
#                    -DNETBENCH_ROUTER='"tcp/192.168.2.90:7447"'
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(netbench)

target_compile_definitions(app PRIVATE
  NETBENCH_WIFI_SSID="${NETBENCH_WIFI_SSID}"
  NETBENCH_WIFI_PASSWORD="${NETBENCH_WIFI_PASSWORD}"
  NETBENCH_ROUTER="${NETBENCH_ROUTER}")

target_sources(app PRIVATE src/main.c)
