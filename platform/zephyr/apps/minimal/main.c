/* SPDX-License-Identifier: Apache-2.0 */
/* R02 最小监督镜像：无智脑也能启动、保持安全并显式关输出。
 * 装配顺序：绑定实例时钟 → 监督执行器（estop → 全通道安全起始态 →
 * WDT → 就绪）→ 常驻空闲。业务步骤由后续批次尾部追加。 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"

static entelea_clock_t platform_clock;

int main(void)
{
	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK) {
		/* 时钟绑定失败 = 启动前置失败：fail-safe（不半启动；
		 * reason 采用 BOOT(0) 语义：第 0 步前的装配失败）。 */
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0; /* 仅 ENTELEA_TEST 构建会走到这里 */
	}

	printk("[entelea] minimal supervision image: outputs held at safe poweron\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
	{
		uint32_t reason = 0;
		bool present = false;

		entelea_storage_last_reset(&reason, &present);
		if (present) {
			printk("[entelea] last abnormal reset reason=0x%08x (noinit)\n",
			       reason);
		}
	}
	for (;;) {
		k_sleep(K_FOREVER); /* main 转常驻监督空闲 */
	}
	return 0;
}
