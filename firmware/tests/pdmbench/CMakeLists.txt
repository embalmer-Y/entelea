# SPDX-License-Identifier: Apache-2.0
# pdmbench（V0.4.x M17b/Q-36）：PDM 麦克风驱动 bring-up 载体（板级，非 twister）。
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pdmbench)
target_sources(app PRIVATE src/main.c)
