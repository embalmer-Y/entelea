/* SPDX-License-Identifier: Apache-2.0 */
/* r14 TF SDMMC 脚位对拍探针。判据行 [hw-tf]：
 *   DET=<0|1>            卡在位（ACTIVE_HIGH：1=卡在位；r14 真机实证
 *                        插卡=开关断开=物理高）
 *   PASS: sd_init ok ...  卡级初始化成功
 *   INIT_FAIL rc=<...>    卡级初始化失败
 * 对拍结论（真机三轮）：clk/cmd 值互换两轮均 PASS（角色由焊盘 func0
 * 硬定）；属性全删即 INIT_FAIL(-ENOTSUP=焊盘未使能)——属性决定使能
 * 哪组焊盘、语义标签不路由信号。见板 dts 注释。 */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	const struct device *sdhc =
		DEVICE_DT_GET(DT_NODELABEL(sdhc0));
	static const struct gpio_dt_spec det =
		GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sd_det_gpios);
	struct sd_card card;
	int rc;

	if (!device_is_ready(sdhc)) {
		printk("[hw-tf] FAIL: sdhc0 not ready\n");
		return 0;
	}
	if (!gpio_is_ready_dt(&det) ||
	    gpio_pin_configure_dt(&det, GPIO_INPUT) != 0) {
		printk("[hw-tf] FAIL: det gpio\n");
		return 0;
	}
	printk("[hw-tf] DET=%d\n", gpio_pin_get_dt(&det) > 0);
	k_sleep(K_SECONDS(1)); /* 卡上电稳定窗 */
	memset(&card, 0, sizeof(card));
	rc = sd_init(sdhc, &card);
	if (rc == 0) {
		printk("[hw-tf] PASS: sd_init ok type=%d ver=%d blocks=%u\n",
		       (int)card.type, (int)card.sd_version,
		       card.block_count);
	} else {
		printk("[hw-tf] INIT_FAIL rc=%d\n", rc);
	}
	return 0;
}
