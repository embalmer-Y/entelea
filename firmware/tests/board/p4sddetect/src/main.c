// SPDX-License-Identifier: Apache-2.0
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define WINDOW_MS 300000
#define DEBOUNCE_MS 80

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_PATH(zephyr_user), sd_detect_gpios),
			  DT_NODELABEL(gpio1)) &&
		     DT_GPIO_PIN(DT_PATH(zephyr_user), sd_detect_gpios) == 13,
	     "This diagnostic only permits GPIO45");

static const struct gpio_dt_spec detect =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sd_detect_gpios);

static int present_from_raw(int raw)
{
	return (detect.dt_flags & GPIO_ACTIVE_LOW) ? !raw : raw;
}

int main(void)
{
	int ret;

	printk("P4SDDETECT START gpio=45 input_only=1 sd_host=0 filesystem=0\n");
	if (!gpio_is_ready_dt(&detect)) {
		printk("P4SDDETECT FAIL gpio_not_ready\n");
		return 1;
	}
	ret = gpio_pin_configure_dt(&detect, GPIO_INPUT | GPIO_PULL_UP);
	if (ret) {
		printk("P4SDDETECT FAIL configure rc=%d\n", ret);
		return 1;
	}
	k_msleep(100);
	int initial = gpio_pin_get_raw(detect.port, detect.pin);

	if (initial < 0) {
		printk("P4SDDETECT FAIL initial_read rc=%d\n", initial);
		return 1;
	}
	int pad = gpio_ll_get_level(&GPIO, 45);

	printk("P4SDDETECT INPUT api_raw=%d pad_raw=%d\n", initial, pad);
	if (initial != pad) {
		printk("P4SDDETECT FAIL gpio_api_mismatch\n");
		return 1;
	}
	int stable = initial;
	int candidate = initial;
	unsigned int edges = 0;
	int64_t started = k_uptime_get();
	int64_t candidate_since = started;

	printk("P4SDDETECT READY raw=%d present=%d window_ms=%d debounce_ms=%d\n",
	       initial, present_from_raw(initial), WINDOW_MS, DEBOUNCE_MS);
	while (k_uptime_get() - started < WINDOW_MS) {
		int raw = gpio_pin_get_raw(detect.port, detect.pin);
		int64_t now = k_uptime_get();

		if (raw < 0) {
			printk("P4SDDETECT FAIL read rc=%d edges=%u\n", raw, edges);
			return 1;
		}
		if (raw != candidate) {
			candidate = raw;
			candidate_since = now;
		} else if (raw != stable && now - candidate_since >= DEBOUNCE_MS) {
			stable = raw;
			edges++;
			printk("P4SDDETECT EDGE index=%u raw=%d present=%d elapsed_ms=%lld\n",
			       edges, raw, present_from_raw(raw), (long long)(now - started));
			if (edges == 2) {
				printk("P4SDDETECT DONE edges=2 initial_raw=%d final_raw=%d\n",
				       initial, stable);
				return 0;
			}
		}
		k_msleep(20);
	}
	printk("P4SDDETECT ABORT timeout edges=%u\n", edges);
	return 1;
}
