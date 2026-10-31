# SPDX-License-Identifier: Apache-2.0
# auditbench（V2-2 / DEC-52①）：审计 flash 环掉电留存真机验证——无网无 APP。
# 判据（AB* console 行）：
#   AB0 start（轮次/复位后 boot_report 已由 audit_init 步打印）
#   AB1 written=n（n 条异常审计落 flash：E_RANGE 限幅 + estop 沿）
#   AB2 readback=n（读回条数与 seq 连续性）
#   AB3 seen=n（复位后 boot_report 打印的历史条数 = 留存实证）
#   AB PASS（复位轮读回 == 首轮写入 + 新增；循环 2 轮后停）
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(auditbench)

target_sources(app PRIVATE src/main.c)
