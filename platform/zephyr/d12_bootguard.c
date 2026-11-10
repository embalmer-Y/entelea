/* SPDX-License-Identifier: Apache-2.0 */
/* D12 平台启动防护（owner v1 保守方案 §9）：__noinit 跨复位异常计数
 * （magic 校验防未定初值）+ hwinfo 复位原因分类 → 异常复位时
 * ①计风暴 ②通知 d12 业务冷却窗；1 小时语义在无 RTC 板上如实降级为
 * **连续异常复位计数**（正常复位清零）≥3 → 第 4 次启动进安全停机
 * 模式（SAFE-HALT：输出保持安全起始态、不启业务、常驻喂狗、
 * 打印 [d12] RESET-STORM 等命令——到场/命令复位可出）。
 * 返回 true=SAFE-HALT（装配层不跑业务步骤）。 */
#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <entelea/d12.h>
#include <zephyr/drivers/hwinfo.h>

#define D12_MAGIC 0x4431424DU /* "D12M" */
#define D12_STORM_MAX 3U

struct d12_noinit {
	uint32_t magic;
	uint32_t count;
};

static __noinit struct d12_noinit guard;

static bool reset_is_abnormal(uint32_t *cause)
{
	if (hwinfo_get_reset_cause(cause) != 0) {
		return true; /* 读不出=保守按异常 */
	}
	hwinfo_clear_reset_cause();
	/* 异常类：WDT/panic/软件 fail-safe；正常类：上电 POR/软复位
	 * （转入正常路径即清零风暴计数）。 */
	return (*cause & (RESET_WATCHDOG | RESET_SOFTWARE | RESET_POR)) !=
	       RESET_POR;
}

static bool safe_halt;

bool entelea_platform_d12_safe_halt(void)
{
	return safe_halt;
}

bool entelea_platform_d12_bootguard(void)
{
	uint32_t cause = 0;
	bool abnormal;

	if (guard.magic != D12_MAGIC) {
		guard.magic = D12_MAGIC;
		guard.count = 0;
	}
	abnormal = reset_is_abnormal(&cause);
	if (!abnormal) {
		guard.count = 0; /* 正常复位出风暴窗 */
	}
	entelea_d12_boot_note(abnormal);
	if (abnormal) {
		guard.count++;
		printk("[d12] abnormal reset #%u cause=0x%x\n",
		       guard.count, cause);
	}
	if (guard.count > D12_STORM_MAX) {
		safe_halt = true;
		printk("[d12] RESET-STORM SAFE-HALT: >=%u consecutive abnormal "
		       "resets; outputs at safe poweron, business halted "
		       "(power-cycle or command to exit)\n",
		       D12_STORM_MAX);
		return true;
	}
	return false;
}
