/* SPDX-License-Identifier: Apache-2.0 */
/* R02 平台装配启动表：estop GPIO → 全通道安全起始态 → WDT 启动 →
 * 监督就绪（占位锚定顺序）。后续业务批次只在尾部追加步骤。 */
#include <zephyr/toolchain.h> /* __weak */
#include <entelea/safety.h>
#include <entelea/storage.h>
#include <entelea/wdt.h>
#include "boot_steps.h"

/* 板级 flash 后端弱桩（R03 板级批）：默认无 flash 面（RAM 后端）；
 * 链接 platform/zephyr/flash_store.c 的目标获得强实现。 */
__weak entelea_result_t entelea_platform_storage_mount_flash(void)
{
	return ENTELEA_E_UNINIT;
}

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

static entelea_result_t step_storage_init(void)
{
	/* R03 尾部追加：存储初始化 + noinit 消费；上次异常复位留痕经
	 * entelea_storage_last_reset 由装配/观测面上报（打印留在平台层）。
	 * 板级批：先挂 flash 后端（弱桩=RAM 直通）再 init。 */
	(void)entelea_platform_storage_mount_flash();
	return entelea_storage_init();
}

const entelea_boot_step_t entelea_platform_boot_steps[ENTELEA_PLATFORM_BOOT_STEP_COUNT] = {
	{"estop_gpio", step_estop_gpio},
	{"safety_poweron", step_safety_poweron},
	{"wdt_start", step_wdt_start},
	{"supervision_ready", step_supervision_ready},
	{"storage_init", step_storage_init},
};
