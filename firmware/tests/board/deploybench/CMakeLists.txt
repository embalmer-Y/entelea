# SPDX-License-Identifier: Apache-2.0
# deploybench（板级十）：Agent→真机完整部署 E2E 的板侧载体。
# 凭证与路由地址经 cmake 变量注入（本地脚本传入，不入仓库——netbench 同纪律）：
#   west build ... -- -DDEPLOYBENCH_WIFI_SSID=... -DDEPLOYBENCH_WIFI_PASSWORD=... \
#                    -DDEPLOYBENCH_ROUTER='"tcp/192.168.2.90:9955"'
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(deploybench)

target_compile_definitions(app PRIVATE
  DEPLOYBENCH_WIFI_SSID="${DEPLOYBENCH_WIFI_SSID}"
  DEPLOYBENCH_WIFI_PASSWORD="${DEPLOYBENCH_WIFI_PASSWORD}"
  DEPLOYBENCH_ROUTER="${DEPLOYBENCH_ROUTER}")

target_sources(app PRIVATE src/main.c)
