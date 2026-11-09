# SPDX-License-Identifier: Apache-2.0
# r14 TF SDMMC 脚位对拍探针：真机三轮实证（clk/cmd 互换两轮 PASS=
# 角色由焊盘硬定；全删属性 INIT_FAIL=属性管使能面），板 dts 注释同源。
# DET=GPIO45 判卡在位（外部 10K 上拉；binding 无 cd 面，应用层读）。
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(entelea_hw_tf_probe)

set(ENTELEA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
zephyr_include_directories(${ENTELEA_ROOT}/framework/include
                           ${ENTELEA_ROOT}/platform/zephyr)
target_sources(app PRIVATE
  main.c
  ${ENTELEA_ROOT}/platform/zephyr/boot_steps.c
  ${ENTELEA_ROOT}/platform/zephyr/clock.c
  ${ENTELEA_ROOT}/framework/common/time.c
  ${ENTELEA_ROOT}/framework/services/storage/part.c
  ${ENTELEA_ROOT}/framework/services/storage/meta.c
  ${ENTELEA_ROOT}/framework/services/storage/noinit.c
  ${ENTELEA_ROOT}/framework/services/storage/audit.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/events.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/wdt.c
  ${ENTELEA_ROOT}/framework/runtime/supervision/boot.c
  ${ENTELEA_ROOT}/framework/execution/safety/channel.c
  ${ENTELEA_ROOT}/framework/execution/safety/commit.c
  ${ENTELEA_ROOT}/framework/execution/safety/force.c
  ${ENTELEA_ROOT}/framework/execution/safety/drivers.c
)
