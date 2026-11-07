// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <zephyr/drivers/i2c.h>

#include "probe.h"

#define CST820_ADDRESS 0x15
#define CST820_ID_REG 0xa7
#define CST820_ID 0xb7

int p4_touch_id_probe(const struct device *bus, uint8_t *id)
{
	int ret;

	if (!id) {
		return -EINVAL;
	}
	if (!device_is_ready(bus)) {
		return -ENODEV;
	}
	ret = i2c_reg_read_byte(bus, CST820_ADDRESS, CST820_ID_REG, id);
	if (ret) {
		return ret;
	}
	return *id == CST820_ID ? 0 : -ENOTSUP;
}
