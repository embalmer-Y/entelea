# SPDX-License-Identifier: Apache-2.0
# atkbench（K1）：ATK DNESP32-S3B V1.1 bring-up + 第二 cube 部署链载体
# （K 批主载体：ES8311 音频 / 信任链 E2E / SD 资源推送 / 多真机 LN）。
# 凭证与路由地址经 cmake 变量注入（本地脚本传入，不入仓库——deploybench 同纪律）：
#   west build ... -- -DATKBENCH_WIFI_SSID=... -DATKBENCH_WIFI_PASSWORD=... \
#                    -DATKBENCH_ROUTER='"tcp/192.168.2.91:9955"'
cmake_minimum_required(VERSION 3.20.5)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(atkbench)

# F3-1 空凭据守卫（真机回归链教训）：SSID 为空 = 烤入空串 → net_mgmt
# 校验 -22 → WiFi 永不关联 → NET wdt 复位循环（症状与硬件无关——
# 归因期背光/lcd_cam/堆假设连环误判，实为构建注入空）。直接失败构建。
if(NOT DEFINED ATKBENCH_WIFI_SSID OR ATKBENCH_WIFI_SSID STREQUAL "")
  message(FATAL_ERROR "ATKBENCH_WIFI_SSID 为空——用 -DATKBENCH_WIFI_SSID=... 注入（仓外凭据纪律）")
endif()

target_compile_definitions(app PRIVATE
  ATKBENCH_WIFI_SSID="${ATKBENCH_WIFI_SSID}"
  ATKBENCH_WIFI_PASSWORD="${ATKBENCH_WIFI_PASSWORD}"
  ATKBENCH_ROUTER="${ATKBENCH_ROUTER}")
# TLS-E2E 助推旗标（外设失联降级告警——缺省不定义 = 判据不变；plan v1.104）
if(DEFINED ATKBENCH_SKIP_PERIPH_FAIL)
  target_compile_definitions(app PRIVATE ATKBENCH_SKIP_PERIPH_FAIL=1)
endif()

target_sources(app PRIVATE src/main.c src/ui_lcd.c)
