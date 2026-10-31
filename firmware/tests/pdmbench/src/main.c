/* SPDX-License-Identifier: Apache-2.0 */
/* pdmbench（V0.4.x M17b/Q-36）：PDM 驱动 bring-up 判据载体。
 * 判据（PDMB PASS）= 连续 8 帧采集：len 恒 640、非全零、非饱和
 * （|max| < 30000）、min != max（信号活动）。PDMB2 逐帧 min/max/mean
 * 供人工判读（静音=小波动、吹气/敲击=大幅波动）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/hal.h>

#define FRAMES 8

static uint8_t frame[640];

int main(void)
{
	printk("PDMB0 pdmbench start\n");
	if (ts_drv_pdm_init() != 0) {
		printk("PDMB FAIL init\n");
		return 1;
	}
	printk("PDMB1 init ok\n");
	int16_t gmin = 32767, gmax = -32768;

	for (int i = 0; i < FRAMES; i++) {
		int n = ts_drv_pdm_capture(frame, sizeof(frame));

		if (n != 640) {
			printk("PDMB FAIL frame %d rc=%d（无帧/超时——时钟或引脚位）\n",
			       i, n);
			return 1;
		}
		int16_t mn = 32767, mx = -32768;
		int64_t sum = 0;
		const int16_t *s = (const int16_t *)frame;

		for (int j = 0; j < 320; j++) {
			if (s[j] < mn) mn = s[j];
			if (s[j] > mx) mx = s[j];
			sum += s[j];
		}
		if (mn < gmin) gmin = mn;
		if (mx > gmax) gmax = mx;
		printk("PDMB2 f=%d len=%d min=%d max=%d mean=%d\n",
		       i, n, mn, mx, (int)(sum / 320));
		k_sleep(K_MSEC(100));
	}
	bool alive = (gmin != gmax) && (gmax < 30000) && (gmin > -30000)
		     && !(gmin == 0 && gmax == 0);

	printk("PDMB3 span=[%d,%d] alive=%d\n", gmin, gmax, (int)alive);
	printk(alive ? "PDMB PASS\n" : "PDMB FAIL flat/saturated\n");
	for (;;) {
		k_sleep(K_SECONDS(5));
		printk("PDMB idle\n");
	}
	return 0;
}
