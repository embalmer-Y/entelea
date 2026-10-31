# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tessera_p4bench)

# P4-W2（V0.14.0）网络面：凭据经 cmake 凭据文件注入（仓外纪律——命令行
# -D 会进 west 错误回显〔p4wifibench 实测教训〕，故走 include 文件，600）：
#   west build ... -- -DEXTRA_CONF_FILE=p4net.conf \
#                    -DP4BENCH_CREDS_FILE=/tmp/p4b_creds.cmake
# 凭据文件形态：set(P4BENCH_WIFI_SSID "...") + set(P4BENCH_WIFI_PASSWORD "...")
# 提供凭据 = 网络面激活（P4BENCH_NET 编入 main.c WiFi/DHCP 块 + P4W2* 判据行）；
# 不提供 = p4bench 既有无网持久链形态（TS_NET=n 基线零变化）。
if(DEFINED P4BENCH_CREDS_FILE)
  include(${P4BENCH_CREDS_FILE})
endif()
if(P4BENCH_WIFI_SSID)
  target_compile_definitions(app PRIVATE
    P4BENCH_NET=1
    P4BENCH_WIFI_SSID="${P4BENCH_WIFI_SSID}"
    P4BENCH_WIFI_PASSWORD="${P4BENCH_WIFI_PASSWORD}")
endif()

target_sources(app PRIVATE src/main.c)
