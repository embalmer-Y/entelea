/* SPDX-License-Identifier: Apache-2.0
 * F3-1（DEC-62②）：atkbench LCD 呈现面实现（LVGL 9.5 @ Zephyr 4.5 glue）。
 *
 * 硬件链（树内板事实 + K2 域）：
 *   ST7789V 320x240 并口（esp32 lcd_cam MIPI-DBI 8080 8-bit——板 DTS
 *   chosen zephyr,display 已挂，驱动 CONFIG_ST7789V/MIPI_DBI_ESP32 随
 *   DT 节点默认 y）；背光 = LCD_BL（XL9555 port0 bit7 高有效——Awinic
 *   交错寄存器图 OP0=0x02/CF0=0x06，与 es8311.c SPK_EN 的 OP1=0x03/
 *   CF1=0x07 同图）。pca9555 树内驱动在本载体禁用（K2 缓存互踩规避），
 *   背光同走裸 I2C。
 *
 * LVGL 线程纪律：lv_* 仅 UI 线程（prio 12——低于 APP 10/net 8，F6 教训：
 * UI 永不抢占业务线程）；跨线程经 ak_ui_screen 入队。
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include <lvgl.h>

#include "ui_lcd.h"

#define AK_UI_LINES 12
#define AK_UI_LINE_LEN 40
#define AK_UI_TITLE_LEN 32

#define XL9555_ADDR 0x20
#define XL9555_OP0 0x02 /* 输出寄存器 port0（Awinic 交错图） */
#define XL9555_CF0 0x06 /* 方向寄存器 port0（1=输入） */

struct ak_ui_msg {
	uint8_t n;
	char title[AK_UI_TITLE_LEN];
	char line[AK_UI_LINES][AK_UI_LINE_LEN];
};

K_MSGQ_DEFINE(ak_ui_q, sizeof(struct ak_ui_msg), 2, 4);

static K_THREAD_STACK_DEFINE(ak_ui_stack, 4096);
static struct k_thread ak_ui_tid;
static lv_obj_t *ak_title;
static lv_obj_t *ak_body;
static lv_obj_t *ak_status;
static atomic_t ak_ui_on;

static int ak_bl_on(void)
{
	const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));

	if (!device_is_ready(i2c)) {
		return -ENODEV;
	}
	/* P0-7 → 输出方向，再置高（LCD_BL 高有效——板 DTS lcd_bl regulator） */
	if (i2c_reg_update_byte(i2c, XL9555_ADDR, XL9555_CF0, BIT(7), 0) != 0 ||
	    i2c_reg_update_byte(i2c, XL9555_ADDR, XL9555_OP0, BIT(7), BIT(7)) != 0) {
		return -EIO;
	}
	return 0;
}

int ak_ui_screen(const char *title, const char *const *lines, uint8_t n)
{
	if (!atomic_get(&ak_ui_on)) {
		return -EAGAIN;
	}
	if (title == NULL || lines == NULL || n == 0 || n > AK_UI_LINES) {
		return -EINVAL;
	}

	struct ak_ui_msg m = {.n = n};

	strncpy(m.title, title, AK_UI_TITLE_LEN - 1);
	for (uint8_t i = 0; i < n; i++) {
		strncpy(m.line[i], lines[i], AK_UI_LINE_LEN - 1);
	}
	return (k_msgq_put(&ak_ui_q, &m, K_NO_WAIT) == 0) ? 0 : -ENOMSG;
}

static void ak_ui_apply(const struct ak_ui_msg *m)
{
	lv_label_set_text(ak_title, m->title);

	static char body[AK_UI_LINES * AK_UI_LINE_LEN];

	body[0] = '\0';
	for (uint8_t i = 0; i < m->n; i++) {
		strcat(body, m->line[i]);
		strcat(body, "\n");
	}
	lv_label_set_text(ak_body, body);
}

void ak_ui_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct ak_ui_msg m;
	uint32_t last_status = UINT32_MAX;

	for (;;) {
		if (k_msgq_get(&ak_ui_q, &m, K_MSEC(50)) == 0) {
			ak_ui_apply(&m);
		}

		/* 状态行（1Hz）：心跳节拍 + 队列深度——屏上可见「UI 活着」 */
		uint32_t now = k_uptime_get_32() / 1000;

		if (now != last_status) {
			last_status = now;
			static char st[AK_UI_LINE_LEN];

			snprintf(st, sizeof(st), "UI up %us", (unsigned)now);
			lv_label_set_text(ak_status, st);
		}
		lv_timer_handler();
	}
}

int ak_ui_init(void)
{
	int rc = ak_bl_on();

	if (rc != 0) {
		return rc;
	}

	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		return -ENODEV;
	}

	struct display_capabilities cap;

	display_get_capabilities(disp, &cap);
	display_blanking_off(disp);

	/* 开机画面：三段布局（标题/正文/状态行）——LVGL 9 屏幕 API */
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);

	ak_title = lv_label_create(scr);
	lv_obj_set_style_text_color(ak_title, lv_color_hex(0x62d6ff), 0);
	lv_label_set_text(ak_title, "TESSERA atkbench");
	lv_obj_align(ak_title, LV_ALIGN_TOP_LEFT, 8, 6);

	ak_body = lv_label_create(scr);
	lv_obj_set_style_text_color(ak_body, lv_color_hex(0xd8d8d8), 0);
	lv_label_set_text(ak_body,
			  "F3-1 LCD bring-up\n"
			  "ST7789V lcd_cam parallel\n"
			  "LVGL 9.5");
	lv_obj_align(ak_body, LV_ALIGN_TOP_LEFT, 8, 34);

	ak_status = lv_label_create(scr);
	lv_obj_set_style_text_color(ak_status, lv_color_hex(0x707070), 0);
	lv_label_set_text(ak_status, "UI up 0s");
	lv_obj_align(ak_status, LV_ALIGN_BOTTOM_LEFT, 8, -6);

	atomic_set(&ak_ui_on, 1);
	k_thread_create(&ak_ui_tid, ak_ui_stack, K_THREAD_STACK_SIZEOF(ak_ui_stack),
			ak_ui_thread_fn, NULL, NULL, NULL, 12, 0, K_NO_WAIT);

	return (cap.x_resolution && cap.y_resolution) ? 0 : -ENODEV;
}
