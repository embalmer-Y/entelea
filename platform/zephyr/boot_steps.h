/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_BOOT_STEPS_H
#define ENTELEA_PLATFORM_BOOT_STEPS_H

/* R02 平台装配：监督启动顺序表（顺序是规格，只允许尾部追加，不可调换
 * 既有次序；ztest 守卫见 platform/zephyr/tests/core）。
 * 旧 M1 四步映射：estop_gpio/safety_poweron/wdt_start 保留原名，
 * core_init 占位改名为 supervision_ready（内容仍为顺序锚定占位：
 * 事件/时钟为静态绑定，无动态 init；绑定由装配 main 在执行器前完成）。 */

#include <stdbool.h>
#include <stddef.h>
#include <entelea/supervision.h>

#define ENTELEA_PLATFORM_BOOT_STEP_COUNT 6

extern const entelea_boot_step_t
	entelea_platform_boot_steps[ENTELEA_PLATFORM_BOOT_STEP_COUNT];

/* D12 复位风暴 SAFE-HALT（d12_bootguard.c 强实现；未链接防护的目标
 * 弱桩恒 false）。装配层在 boot 后查询：true=不启业务、输出保持安全
 * 起始态、常驻喂狗（POR 出）。业务板级镜像必须链接防护。 */
bool
entelea_platform_d12_bootguard(void);
bool
entelea_platform_d12_safe_halt(void);

#ifdef ENTELEA_TEST
void
entelea_platform_d12_test_reset(void);
#endif

#endif /* ENTELEA_PLATFORM_BOOT_STEPS_H */
