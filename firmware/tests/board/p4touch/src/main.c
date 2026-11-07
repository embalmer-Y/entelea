// SPDX-License-Identifier: Apache-2.0
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "events.h"
#include "probe.h"

#define TOUCH_DEVICE DEVICE_DT_GET(DT_NODELABEL(p4_touch))
#define TOUCH_PAIRS 5
#define TOUCH_TIMEOUT_SECONDS 300

static K_SEM_DEFINE(completed, 0, TOUCH_PAIRS);
static K_SEM_DEFINE(armed, 0, 1);
static atomic_t running;
static atomic_t failed;
static struct touch_state state;
static bool aligned;

static int report_raw(const struct device *bus, const char *phase)
{
	uint8_t report[7];
	int ret;

	ret = i2c_burst_read(bus, 0x15, 0x00, report, sizeof(report));
	if (ret) {
		printk("P4TOUCH FAIL raw phase=%s rc=%d\n", phase, ret);
		return ret;
	}
	printk("P4TOUCH RAW phase=%s data=%02x,%02x,%02x,%02x,%02x,%02x,%02x\n", phase, report[0],
	       report[1], report[2], report[3], report[4], report[5], report[6]);
	return 0;
}

static void touch_callback(struct input_event *event, void *user_data)
{
	int result;

	ARG_UNUSED(user_data);
	if (!atomic_get(&running)) {
		return;
	}
	if (!aligned) {
		if (event->type == INPUT_EV_KEY && event->code == INPUT_BTN_TOUCH &&
		    event->value == 0) {
			aligned = true;
			k_sem_give(&armed);
		}
		return;
	}
	result = touch_event_apply(&state, event->type, event->code, event->value);
	if (result < 0) {
		printk("P4TOUCH FAIL event rc=%d x=%d y=%d\n", result, state.x, state.y);
		atomic_set(&failed, 1);
		k_sem_give(&completed);
	} else if (result == TOUCH_DOWN) {
		printk("P4TOUCH DOWN x=%d y=%d\n", state.x, state.y);
	} else if (result == TOUCH_MOVE) {
		printk("P4TOUCH MOVE x=%d y=%d\n", state.x, state.y);
	} else if (result == TOUCH_UP) {
		printk("P4TOUCH UP\n");
		k_sem_give(&completed);
	}
}

INPUT_CALLBACK_DEFINE(TOUCH_DEVICE, touch_callback, NULL);

int main(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	uint8_t id = 0;
	int ret;
	int64_t deadline;

	printk("P4TOUCH START display=0 reset=0 poll_ms=50\n");
	ret = p4_touch_id_probe(bus, &id);
	if (ret) {
		printk("P4TOUCH FAIL id rc=%d\n", ret);
		return 1;
	}
	ret = report_raw(bus, "before_init");
	if (ret) {
		return 1;
	}
	ret = device_init(TOUCH_DEVICE);
	if (ret) {
		printk("P4TOUCH FAIL init rc=%d\n", ret);
		return 1;
	}
	if (!device_is_ready(TOUCH_DEVICE)) {
		printk("P4TOUCH FAIL device not ready\n");
		return 1;
	}
	atomic_set(&running, 1);
	if (k_sem_take(&armed, K_SECONDS(3))) {
		atomic_clear(&running);
		report_raw(bus, "idle_timeout");
		printk("P4TOUCH FAIL waiting for idle event\n");
		return 1;
	}
	if (atomic_get(&failed)) {
		atomic_clear(&running);
		return 1;
	}
	printk("P4TOUCH READY id=0x%02x pairs=%d timeout_s=%d\n", id, TOUCH_PAIRS,
	       TOUCH_TIMEOUT_SECONDS);
	deadline = k_uptime_get() + TOUCH_TIMEOUT_SECONDS * 1000;
	for (int i = 0; i < TOUCH_PAIRS; i++) {
		int64_t remaining = deadline - k_uptime_get();

		if (remaining <= 0 || k_sem_take(&completed, K_MSEC(remaining))) {
			printk("P4TOUCH FAIL timeout completed=%d\n", i);
			atomic_clear(&running);
			return 1;
		}
		if (atomic_get(&failed)) {
			atomic_clear(&running);
			return 1;
		}
		printk("P4TOUCH PAIR %d/%d\n", i + 1, TOUCH_PAIRS);
	}
	atomic_clear(&running);
	printk("P4TOUCH PASS pairs=%d\n", TOUCH_PAIRS);
	return 0;
}
