// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "probe.h"
#ifdef CONFIG_TS_P4_BOOT_SOFT_RESETS
#include "boot_trace.h"
#endif

int main(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	uint8_t id = 0;
	int ret;

	printk("P4TOUCHID START read_only=1 reset=0 display=0\n");
	ret = p4_touch_id_probe(bus, &id);
	if (ret == -ENOTSUP) {
		printk("P4TOUCHID FAIL unexpected_id=0x%02x\n", id);
	} else if (ret) {
		printk("P4TOUCHID FAIL read rc=%d\n", ret);
	} else {
		printk("P4TOUCHID PASS id=0x%02x\n", id);
#ifdef CONFIG_TS_P4_BOOT_SOFT_RESETS
		ret = ts_p4_boot_soft_resets();
#endif
	}
	return ret ? 1 : 0;
}
