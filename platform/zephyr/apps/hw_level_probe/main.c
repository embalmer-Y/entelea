/* SPDX-License-Identifier: Apache-2.0 */
/* r14 电平测量探针（r02 遗留到场项闭环）：安全通道 GPIO 输出后端
 * （ENTELEA_DRV_GPIO 真路径）首次板级物理电平验证。串口节拍叙事
 * 引导万用表测量，判据行 [hw-lvl]：
 *   P1 OUT=HIGH  J4-32 预期 ~3.3V
 *   P2 OUT=LOW   J4-32 预期 ~0V
 *   P3 estop 触发（GPIO20 插 GND）→ 输出物理坍缩 ~0V（核心判据：
 *   软件安全语义落到物理电压）
 * estop 闩后循环只报 LATCHED 保持 ~0V（EN 复位重启循环）。 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/time.h>
#include <entelea/wdt.h>
#include "clock.h"

/* drivers.c 输出后端装配入口（内部面；无 DT uid/io-gpios 构建=桩）。 */
extern int entelea_drv_gpio_init(void);

static entelea_clock_t platform_clock;

static const entelea_out_channel_t probe_ch = {
	.uid = "probe1",
	.kind = ENTELEA_CH_GPIO,
	.poweron = {.b = false},
	.linkloss = {.b = false},
	.fault = {.b = false},
};

static void phase(const char *s, int secs)
{
	printk("[hw-lvl] %s (%ds window)\n", s, secs);
	k_sleep(K_SECONDS(secs));
}

int main(void)
{
	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_estop_init() != ENTELEA_OK ||
	    entelea_drv_gpio_init() != 0 ||
	    entelea_safety_register_channel(&probe_ch) != ENTELEA_OK ||
	    entelea_safety_poweron_init() != ENTELEA_OK) {
		printk("[hw-lvl] FAIL: init chain\n");
		return 0;
	}
	(void)entelea_safety_set_link(true);
	(void)entelea_safety_channel_recover("probe1");
	printk("[hw-lvl] init ok: probe1 ACTIVE on J4-32 (GPIO21); estop GPIO20\n");

	for (;;) {
		if (entelea_safety_estop_latched()) {
			phase("LATCHED: OUT held at fault value, J4-32 expect ~0V",
			      10);
			continue;
		}
		if (entelea_safety_commit("probe1",
					  (entelea_out_value_t){.b = true})
			    == ENTELEA_OK) {
			phase("P1 OUT=HIGH on J4-32 (GPIO21): expect ~3.3V", 30);
		}
		if (entelea_safety_estop_latched()) {
			continue;
		}
		if (entelea_safety_commit("probe1",
					  (entelea_out_value_t){.b = false})
			    == ENTELEA_OK) {
			phase("P2 OUT=LOW on J4-32 (GPIO21): expect ~0V", 30);
		}
		if (entelea_safety_estop_latched()) {
			continue;
		}
		if (entelea_safety_commit("probe1",
					  (entelea_out_value_t){.b = true})
			    == ENTELEA_OK) {
			phase("P3a OUT=HIGH pre-estop; get ready to touch GPIO20 (J4-31) wire to GND", 20);
			phase("P3b estop window: touch GPIO20->GND now; OUT collapses, J4-32 expect ~0V (J4-31 also ~0V)", 45);
		}
	}
	return 0;
}
