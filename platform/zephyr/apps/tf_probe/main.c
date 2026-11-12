/* SPDX-License-Identifier: Apache-2.0 */
/* r14 TF SDMMC pin cross-check probe. Criteria lines [hw-tf]:
 *   DET=<0|1>            card present (ACTIVE_HIGH: 1=card present; r14
 *                        hardware-verified: card inserted = switch open =
 *                        physically high)
 *   PASS: sd_init ok ...  card-level initialization succeeded
 *   INIT_FAIL rc=<...>    card-level initialization failed
 * Cross-check results (three hardware rounds): swapping clk/cmd values
 * passed in both rounds (roles are fixed by pad func0). Deleting all
 * properties gives INIT_FAIL (-ENOTSUP = pad not enabled). Properties
 * decide which pad group is enabled; semantic labels do not route signals.
 * See the board dts comments. */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sys/printk.h>

int
main(void)
{
	const struct device *sdhc = DEVICE_DT_GET(DT_NODELABEL(sdhc0));
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
	k_sleep(K_SECONDS(1)); /* settle window after card power-up */
	memset(&card, 0, sizeof(card));
	rc = sd_init(sdhc, &card);
	if (rc == 0) {
		printk("[hw-tf] PASS: sd_init ok type=%d ver=%d blocks=%u\n",
		       (int) card.type, (int) card.sd_version,
		       card.block_count);
	} else {
		printk("[hw-tf] INIT_FAIL rc=%d\n", rc);
	}
	return 0;
}
