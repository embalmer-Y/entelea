/* SPDX-License-Identifier: Apache-2.0 */
/* R02 平台装配启动表：estop GPIO → 全通道安全起始态 → WDT 启动 →
 * 监督就绪（占位锚定顺序）。后续业务批次只在尾部追加步骤。 */
#include <entelea/safety.h>
#include <entelea/wdt.h>
#include "boot_steps.h"

static entelea_result_t step_estop_gpio(void)
{
	return entelea_safety_estop_init();
}

static entelea_result_t step_safety_poweron(void)
{
	return entelea_safety_poweron_init();
}

static entelea_result_t step_wdt_start(void)
{
	return entelea_wdt_start();
}

static entelea_result_t step_supervision_ready(void)
{
	/* 顺序锚定占位（旧 core_init 同语义）：事件/时钟为静态绑定，
	 * 由装配 main 在执行器前完成；无动态 init。 */
	return ENTELEA_OK;
}

const entelea_boot_step_t entelea_platform_boot_steps[ENTELEA_PLATFORM_BOOT_STEP_COUNT] = {
	{"estop_gpio", step_estop_gpio},
	{"safety_poweron", step_safety_poweron},
	{"wdt_start", step_wdt_start},
	{"supervision_ready", step_supervision_ready},
};
