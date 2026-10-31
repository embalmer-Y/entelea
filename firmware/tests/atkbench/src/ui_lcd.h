/* SPDX-License-Identifier: Apache-2.0
 * F3-1（DEC-62②）：atkbench LCD 呈现面（LVGL @ ST7789V 320x240 并口）。
 * 设备形态胶水——框架 F3-2 只提供 pending 状态机/事件，本模块是纯呈现。
 * 线程纪律：LVGL 非线程安全——一切 lv_* 调用仅在 UI 线程内；其他线程
 * 经 ak_ui_screen() 入队（k_msgq 编排，投递即返回不阻塞调用方）。
 * 文本面 ASCII（Montserrat 14；CJK 字库 V1 不入——判据行/manifest 字段
 * 均为 ASCII）。
 */
#pragma once

#include <stdint.h>

/* 初始化：背光 XL9555 P0-7 高有效（裸 I2C——pca9555 驱动禁用纪律同
 * es8311.c SPK_EN）+ 默认屏引用 + 开机画面 + UI 线程起搏。
 * 返回 0 = OK；<0 = 如实失败（无屏/背光链路失败不阻塞 bench 主链）。 */
int ak_ui_init(void);

/* 多行文本屏（≤AK_UI_LINES 行，每行截断 AK_UI_LINE_LEN-1）。任意线程
 * 可调；返回 0 = 已入队，-1 = 队满（如实）。 */
int ak_ui_screen(const char *title, const char *const *lines, uint8_t n);

/* UI 线程节拍（lv_timer_handler + 队列应用 + 状态行刷新）——仅 UI 线程
 * 内部使用（ak_ui_init 内 K_THREAD_DEFINE 自起）。 */
void ak_ui_thread_fn(void *p1, void *p2, void *p3);
