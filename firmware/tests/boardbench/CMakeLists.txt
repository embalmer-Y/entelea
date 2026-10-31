# SPDX-License-Identifier: Apache-2.0
# boardbench（板级二效率基准）：非 twister 独立应用——west build -b 手动
# 构建烧录，console 输出为测量记录（方法学与数据见 docs/board-bench.md）。
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(boardbench)

target_sources(app PRIVATE src/main.c)
