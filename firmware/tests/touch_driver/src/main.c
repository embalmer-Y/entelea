// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <string.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/input/input.h>
#include <zephyr/ztest.h>

#define TOUCH DEVICE_DT_GET(DT_NODELABEL(touch))

struct frame {
	int32_t x;
	int32_t y;
	int32_t pressed;
	unsigned int coordinates;
};

static K_SEM_DEFINE(samples, 0, 1);
static K_SEM_DEFINE(read_done, 0, 1);
K_MSGQ_DEFINE(frames, sizeof(struct frame), 4, 4);
static uint8_t registers[256];
static int report_error;
static struct frame current;

static int transfer(const struct emul *target, struct i2c_msg *msgs, int count, int address)
{
	uint8_t reg;

	ARG_UNUSED(target);
	zassert_equal(address, 0x15);
	if (count == 1) {
		zassert_equal(msgs[0].len, 2);
		zassert_false(msgs[0].flags & I2C_MSG_READ);
		registers[msgs[0].buf[0]] = msgs[0].buf[1];
		return 0;
	}
	zassert_equal(count, 2);
	zassert_equal(msgs[0].len, 1);
	reg = msgs[0].buf[0];
	zassert_true(msgs[1].flags & I2C_MSG_READ);
	if (reg >= 2 && reg <= 6) {
		k_sem_take(&samples, K_FOREVER);
		zassert_equal(reg, 2, "Read finger count with coordinates in one transaction");
		zassert_equal(msgs[1].len, 5);
		zassert_true(msgs[1].flags & I2C_MSG_RESTART);
		zassert_true(msgs[1].flags & I2C_MSG_STOP);
		if (!report_error) {
			memcpy(msgs[1].buf, &registers[reg], msgs[1].len);
		}
		k_sem_give(&read_done);
		return report_error;
	}
	zassert_equal(msgs[1].len, 1);
	msgs[1].buf[0] = registers[reg];
	return 0;
}

static const struct i2c_emul_api bus_api = {
	.transfer = transfer,
};

static int emul_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);
	registers[0xa7] = 0xb7;
	return 0;
}

EMUL_DT_DEFINE(DT_NODELABEL(touch), emul_init, NULL, NULL, &bus_api, NULL);

static void input_callback(struct input_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	if (event->type == INPUT_EV_ABS && event->code == INPUT_ABS_X) {
		current.x = event->value;
		current.coordinates++;
	} else if (event->type == INPUT_EV_ABS && event->code == INPUT_ABS_Y) {
		current.y = event->value;
		current.coordinates++;
	} else if (event->type == INPUT_EV_KEY && event->code == INPUT_BTN_TOUCH) {
		current.pressed = event->value;
		zassert_equal(k_msgq_put(&frames, &current, K_NO_WAIT), 0);
		current.coordinates = 0;
	}
}

INPUT_CALLBACK_DEFINE(TOUCH, input_callback, NULL);

static void *setup(void)
{
	zassert_equal(device_init(TOUCH), 0);
	return NULL;
}

static struct frame sample(uint8_t fingers, uint8_t event, uint16_t x, uint16_t y)
{
	struct frame frame;

	registers[2] = fingers;
	registers[3] = (event << 6) | ((x >> 8) & 0x0f);
	registers[4] = x;
	registers[5] = (fingers ? 0xa0 : 0) | ((y >> 8) & 0x0f);
	registers[6] = y;
	report_error = 0;
	k_sem_give(&samples);
	zassert_equal(k_sem_take(&read_done, K_SECONDS(1)), 0);
	zassert_equal(k_msgq_get(&frames, &frame, K_SECONDS(1)), 0);
	return frame;
}

ZTEST(touch_driver, test_idle_zero_report)
{
	struct frame frame = sample(0, 0, 0, 0);

	zassert_equal(frame.pressed, 0);
	zassert_equal(frame.coordinates, 0);
}

ZTEST(touch_driver, test_no_finger_stale_contact)
{
	struct frame frame = sample(0, 2, 459, 459);

	zassert_equal(frame.pressed, 0);
	zassert_equal(frame.coordinates, 0);
}

ZTEST(touch_driver, test_down_and_contact)
{
	for (uint8_t event = 0; event <= 2; event += 2) {
		struct frame frame = sample(1, event, 321, 459);

		zassert_equal(frame.pressed, 1);
		zassert_equal(frame.coordinates, 2);
		zassert_equal(frame.x, 321);
		zassert_equal(frame.y, 459);
	}
}

ZTEST(touch_driver, test_lift_and_none)
{
	for (uint8_t event = 1; event <= 3; event += 2) {
		struct frame frame = sample(1, event, 400, 300);

		zassert_equal(frame.pressed, 0);
		zassert_equal(frame.coordinates, 0);
	}
}

ZTEST(touch_driver, test_out_of_range_is_not_clipped)
{
	struct frame frame = sample(1, 0, 460, 4095);

	zassert_equal(frame.pressed, 1);
	zassert_equal(frame.x, 460);
	zassert_equal(frame.y, 4095);
}

ZTEST(touch_driver, test_bus_error_does_not_report_success)
{
	struct frame frame;

	report_error = -EIO;
	k_sem_give(&samples);
	zassert_equal(k_sem_take(&read_done, K_SECONDS(1)), 0);
	zassert_not_equal(k_msgq_get(&frames, &frame, K_MSEC(30)), 0);
	frame = sample(1, 0, 10, 20);
	zassert_equal(frame.pressed, 1);
	zassert_equal(frame.x, 10);
	zassert_equal(frame.y, 20);
}

ZTEST_SUITE(touch_driver, NULL, setup, NULL, NULL, NULL);
