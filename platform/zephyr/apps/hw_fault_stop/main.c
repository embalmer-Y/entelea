/* SPDX-License-Identifier: Apache-2.0 */
/* R02 D12 硬件闸验收镜像（板级）：真实硅上闭环验证两相故障停止。
 * 相位判别 = hwinfo 复位原因（noinit RAM 后端不跨复位存活——BSS 每次
 * 启动重置，板级 flash 后端归 R03 板级批，故不用于判别）：
 *  P1（上电/外硬复位进入）：健康窗口 6s（巡检+MWDT 喂狗的差分证明：
 *     若 MWDT 未安装/未喂则活不过窗口）→ 生产 system_fail(FAIL_WDT
 *     0xFF) → 停喂 → task_wdt 通道 5s 到期 → sys_reboot（期望复位
 *     原因 = SOFTWARE，≤~5s）。
 *  P2（复位原因 = SOFTWARE 进入）：irq_lock+自旋楔死内核（本板单核
 *     配置，CONFIG_SMP 未置）→ task_wdt 监视器死亡 → MWDT0
 *     (RESET_SOC, 窗 3+2s) 到期 → SoC 硬复位（期望复位原因 =
 *     WATCHDOG；ROM 横幅 rst:0x8 独立佐证）。
 *  P3（复位原因 = WATCHDOG 进入）：双闭环收官 → 常驻（巡检持续喂狗）。
 * 无测试钩子（ENTELEA_TEST 未定义）：只走生产 API。 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/hwinfo.h>
#include <entelea/safety.h>
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
		/* 时钟绑定失败 = 启动前置失败：fail-safe（同 minimal）。 */
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0;
	}
	printk("[hw-gate] boot sequence begins\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);
	/* boot 任一步失败不返回（fail-safe 停机 + WDT 兜底复位）——
	 * 执行到这里 = 五步全过（含 wdt_start 在真 MWDT0 上安装成功）。 */
	uint32_t cause = 0;
	int rc = hwinfo_get_reset_cause(&cause);

	printk("[hw-gate] boot ok (patrol=%ums channel=%ums); hwinfo rc=%d cause=0x%x\n",
	       entelea_wdt_patrol_period_ms(), ENTELEA_WDT_CHANNEL_MS, rc, cause);

	if (rc == 0 && cause == RESET_WATCHDOG) {
		printk("[hw-gate] P2 VERIFIED: hardware MWDT reset the SoC\n");
		printk("[hw-gate] gate complete: parking resident (patrol feeding)\n");
	} else if (rc == 0 && cause == RESET_SOFTWARE) {
		printk("[hw-gate] P1 VERIFIED: system_fail -> channel expiry -> reset\n");
		printk("[hw-gate] P2: wedging kernel now; MWDT reset expected <=~8s\n");
		(void)irq_lock();
		volatile unsigned spin = 0;

		for (;;) {
			spin++;
		}
	} else {
		printk("[hw-gate] P1: healthy 6s window (alive = patrol+MWDT fed)\n");
		for (int i = 1; i <= 6; i++) {
			k_sleep(K_SECONDS(1));
			printk("[hw-gate] alive %ds\n", i);
		}
		printk("[hw-gate] P1: system_fail(FAIL_WDT 0xff); reset expected <=~5s\n");
		entelea_safety_system_fail(ENTELEA_FAIL_WDT(0xFFu));
	}
	for (;;) {
		k_sleep(K_FOREVER); /* 常驻监督空闲（P1 停喂后通道到期触发复位） */
	}
	return 0;
}
