// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/ztest.h>

#include "probe.h"

static int transfer_error;
static uint8_t chip_id;
static unsigned int transfers;

static int fake_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
			 uint16_t address)
{
	ARG_UNUSED(dev);
	zassert_equal(address, 0x15);
	zassert_equal(count, 2);
	zassert_equal(msgs[0].len, 1);
	zassert_equal(msgs[0].buf[0], 0xa7);
	zassert_equal(msgs[0].flags & I2C_MSG_READ, 0);
	zassert_equal(msgs[1].len, 1);
	zassert_true(msgs[1].flags & I2C_MSG_READ);
	zassert_true(msgs[1].flags & I2C_MSG_RESTART);
	zassert_true(msgs[1].flags & I2C_MSG_STOP);
	transfers++;
	if (transfer_error) {
		return transfer_error;
	}
	msgs[1].buf[0] = chip_id;
	return 0;
}

static DEVICE_API(i2c, fake_api) = {
	.transfer = fake_transfer,
};

DEVICE_DEFINE(touch_id_bus, "touch-id-bus", NULL, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_api);

static void reset_test(void *fixture)
{
	ARG_UNUSED(fixture);
	transfer_error = 0;
	chip_id = 0xb7;
	transfers = 0;
}

ZTEST(touch_id, test_cst820)
{
	uint8_t id = 0;

	zassert_equal(p4_touch_id_probe(DEVICE_GET(touch_id_bus), &id), 0);
	zassert_equal(id, 0xb7);
	zassert_equal(transfers, 1);
}

ZTEST(touch_id, test_unexpected_id)
{
	uint8_t id = 0;

	chip_id = 0xb5;
	zassert_equal(p4_touch_id_probe(DEVICE_GET(touch_id_bus), &id), -ENOTSUP);
	zassert_equal(id, 0xb5);
	zassert_equal(transfers, 1);
}

ZTEST(touch_id, test_bus_error)
{
	uint8_t id = 0;

	transfer_error = -EIO;
	zassert_equal(p4_touch_id_probe(DEVICE_GET(touch_id_bus), &id), -EIO);
	zassert_equal(transfers, 1);
}

ZTEST(touch_id, test_unready_bus_and_null_output)
{
	uint8_t id = 0;

	zassert_equal(p4_touch_id_probe(NULL, &id), -ENODEV);
	zassert_equal(p4_touch_id_probe(DEVICE_GET(touch_id_bus), NULL), -EINVAL);
	zassert_equal(transfers, 0);
}

ZTEST_SUITE(touch_id, NULL, NULL, reset_test, NULL, NULL);
