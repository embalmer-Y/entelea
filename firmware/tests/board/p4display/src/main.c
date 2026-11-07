// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdlib.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <hal/dw_gdma_ll.h>
#include <hal/mipi_dsi_brg_ll.h>
#include <hal/mipi_dsi_host_ll.h>

#define WIDTH 460U
#define HEIGHT 460U
#define BRIGHTNESS 32U
#define HBM_BRIGHTNESS 8U
#define HOLD_SECONDS 15U

static const struct device *const host = DEVICE_DT_GET(DT_NODELABEL(mipi_dsi));
static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const supply = DEVICE_DT_GET(DT_NODELABEL(ldo3));
static atomic_t completed_frames;

static const struct mipi_dsi_device panel = {
	.data_lanes = 1,
	.pixfmt = MIPI_DSI_PIXFMT_RGB565,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM,
	.timings.hactive = WIDTH,
	.timings.hsync = 4,
	.timings.hbp = 72,
	.timings.hfp = 32,
	.timings.vactive = HEIGHT,
	.timings.vsync = 2,
	.timings.vbp = 4,
	.timings.vfp = 6,
};

static int command(uint8_t reg, const void *data, size_t length)
{
	ssize_t ret = mipi_dsi_dcs_write(host, 0, reg, data, length);

	if (ret < 0 || ret != length) {
		printk("P4DISPLAY FAIL dcs=0x%02x rc=%d expected=%u\n", reg, (int)ret,
		       (unsigned int)length);
		return ret < 0 ? (int)ret : -EIO;
	}
	return 0;
}

static int byte_command(uint8_t reg, uint8_t value)
{
	return command(reg, &value, sizeof(value));
}

static int panel_init(void)
{
	const uint8_t columns[] = {0, 10, 1, 0xd5};
	const uint8_t rows[] = {0, 0, 1, 0xcb};
	int ret;

	ret = byte_command(0xfe, 0);
	if (!ret) {
		ret = byte_command(0x51, 0);
	}
	if (!ret) {
		ret = byte_command(0x3a, 0x55);
	}
	if (!ret) {
		ret = byte_command(0x53, 0x20);
	}
	if (!ret) {
		ret = byte_command(0x63, HBM_BRIGHTNESS);
	}
	if (!ret) {
		ret = command(0x2a, columns, sizeof(columns));
	}
	if (!ret) {
		ret = command(0x2b, rows, sizeof(rows));
	}
	if (!ret) {
		ret = command(0x11, NULL, 0);
		k_sleep(K_MSEC(120));
	}
	if (!ret) {
		ret = command(0x29, NULL, 0);
	}
	return ret;
}

static enum display_event_result on_frame(const struct device *dev, uint32_t event,
					  const struct display_event_data *data, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(event);
	ARG_UNUSED(data);
	ARG_UNUSED(user_data);
	atomic_inc(&completed_frames);
	return DISPLAY_EVENT_RESULT_CONTINUE;
}

static void link_snapshot(const char *phase)
{
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);

	printk("P4DISPLAY DMA phase=%s intr=%08x cfg0=%08x cfg1=%08x status=%08x\n", phase,
	       dw_gdma_ll_channel_get_intr_status(dma, 0), dma->ch[0].cfg0.val, dma->ch[0].cfg1.val,
	       dma->ch[0].status0.val);
	printk("P4DISPLAY LINK phase=%s src=%08x dst=%08x ctl1=%08x phy=%08x bridge=%08x\n", phase,
	       dma->ch[0].sar0.val, dma->ch[0].dar0.val, dma->ch[0].ctl1.val,
	       MIPI_DSI_LL_GET_HOST(0)->phy_status.val,
	       mipi_dsi_brg_ll_get_interrupt_status(MIPI_DSI_LL_GET_BRG(0)));
}

static void isolate_link(void)
{
	int ret;

	link_snapshot("dma_timeout");
	mipi_dsi_host_ll_dpi_set_pattern_type(MIPI_DSI_LL_GET_HOST(0),
					      MIPI_DSI_PATTERN_BAR_VERTICAL);
	ret = byte_command(0x51, BRIGHTNESS);
	if (!ret) {
		printk("P4DISPLAY ISOLATION host_color_bars seconds=10 acceptance=0\n");
		k_sleep(K_SECONDS(10));
		link_snapshot("host_pattern");
	}
	mipi_dsi_host_ll_dpi_set_pattern_type(MIPI_DSI_LL_GET_HOST(0), MIPI_DSI_PATTERN_NONE);
}

static int show_pattern(bool quadrants)
{
	const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
	const struct display_buffer_descriptor descriptor = {
		.width = WIDTH,
		.height = HEIGHT,
		.pitch = WIDTH,
		.buf_size = WIDTH * HEIGHT * sizeof(uint16_t),
	};
	uint16_t *pixels;
	int ret;
	atomic_val_t before;
	atomic_val_t frames;
	uint32_t dma_words;

	ret = byte_command(0x51, 0);
	if (ret) {
		return ret;
	}
	pixels = display_get_framebuffer(display);
	if (!pixels) {
		printk("P4DISPLAY FAIL framebuffer unavailable\n");
		return -ENOMEM;
	}
	for (unsigned int y = 0; y < HEIGHT; y++) {
		for (unsigned int x = 0; x < WIDTH; x++) {
			unsigned int index = quadrants ? (y >= HEIGHT / 2) * 2 + (x >= WIDTH / 2)
						       : x * 3 / WIDTH;

			pixels[y * WIDTH + x] = colors[index];
		}
	}
	ret = display_write(display, 0, 0, &descriptor, pixels);
	if (ret) {
		printk("P4DISPLAY FAIL present rc=%d\n", ret);
		return ret;
	}
	k_sleep(K_MSEC(100));
	ret = byte_command(0x51, BRIGHTNESS);
	if (ret) {
		return ret;
	}
	before = atomic_get(&completed_frames);
	printk("P4DISPLAY SHOW pattern=%s brightness=%u seconds=%u\n",
	       quadrants ? "quadrants_R_G_B_W" : "bars_R_G_B", BRIGHTNESS, HOLD_SECONDS);
	k_sleep(K_SECONDS(HOLD_SECONDS));
	frames = atomic_get(&completed_frames) - before;
	dma_words = DW_GDMA_LL_GET_HW(0)->ch[0].block_ts0.val + 1;
	printk("P4DISPLAY FRAMES count=%ld seconds=%u dma_words=%u expected=%u\n", (long)frames,
	       HOLD_SECONDS, dma_words, WIDTH * HEIGHT * sizeof(uint16_t) / 8);
	if (dma_words != WIDTH * HEIGHT * sizeof(uint16_t) / 8) {
		printk("P4DISPLAY FAIL DMA transfer length differs from pixel frame\n");
		isolate_link();
		return -EMSGSIZE;
	}
	if (frames < HOLD_SECONDS * 10) {
		printk("P4DISPLAY FAIL insufficient frame completions\n");
		isolate_link();
		return -ETIMEDOUT;
	}
	return 0;
}

int main(void)
{
	bool attached = false;
	uint32_t handle;
	int ret;
	int cleanup;

	printk("P4DISPLAY START reset_gpio=none touch=0 network=0 brightness_max=%u hbm_max=%u\n",
	       BRIGHTNESS, HBM_BRIGHTNESS);
	if (!device_is_ready(host) || !device_is_ready(display) || !device_is_ready(supply)) {
		printk("P4DISPLAY FAIL devices not ready\n");
		return 1;
	}
	ret = mipi_dsi_attach(host, 0, &panel);
	if (ret) {
		printk("P4DISPLAY FAIL attach rc=%d\n", ret);
	} else {
		attached = true;
		ret = display_register_event_cb(display, on_frame, NULL, DISPLAY_EVENT_FRAME_DONE,
						true, &handle);
		if (ret) {
			printk("P4DISPLAY FAIL event callback rc=%d\n", ret);
		}
	}
	if (!ret) {
		ret = panel_init();
	}
	if (!ret) {
		ret = show_pattern(false);
	}
	if (!ret) {
		ret = show_pattern(true);
	}
	cleanup = byte_command(0x51, 0);
	if (cleanup) {
		ret = cleanup;
	}
	cleanup = command(0x28, NULL, 0);
	if (cleanup) {
		ret = cleanup;
	}
	cleanup = command(0x10, NULL, 0);
	k_sleep(K_MSEC(120));
	if (cleanup) {
		ret = cleanup;
	}
	if (attached) {
		cleanup = mipi_dsi_detach(host, 0, &panel);
		if (cleanup) {
			printk("P4DISPLAY FAIL detach rc=%d\n", cleanup);
			ret = cleanup;
		}
	}
	cleanup = regulator_disable(supply);
	if (cleanup) {
		printk("P4DISPLAY FAIL supply disable rc=%d\n", cleanup);
		ret = cleanup;
	}
	printk("P4DISPLAY %s visual_check_required=1\n", ret ? "FAIL" : "IO_OK");
	return ret ? 1 : 0;
}
