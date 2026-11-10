/* SPDX-License-Identifier: Apache-2.0 */
/* D12 平台启动防护（owner v1 保守方案 §9）。跨复位计数走 storage
 * noinit（板级 flash 后端真持久——r16 真机实证：__noinit 段变量在
 * 本板复位路径不可靠，风暴计数每次归零）。hwinfo 复位原因分类：
 * 非纯 POR（WDT/软件 fail-safe）=异常；异常时①计风暴②通知 d12
 * 60s 业务冷却窗。连续异常 ≥3（无 RTC 板：1 小时语义如实降级为
 * 连续计数，正常复位清零）→ 第 4 次启动 SAFE-HALT（输出安全
 * 起始态、不启业务、常驻喂狗、打印 [d12] RESET-STORM——POR 出）。
 * 返回 true=SAFE-HALT（装配层不跑业务；getter 供消费）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/retained_mem.h>
#include <entelea/d12.h>

#define D12_MAGIC 0x4431424DU /* "D12M" */
#define D12_STORM_MAX 3U

struct d12_rec {
	uint32_t magic;
	uint32_t count;
};

/* LP 域 retained RAM 设备（app overlay d12retained 节点；跨 soft/WDT
 * 复位保留、POR 清=风暴窗自然出。r16 实证：__noinit 宏不达 noinit
 * 段、dram0.noinit 是 flash 装载恢复段、storage noinit 槽与
 * last_reset 留痕互踩——retained RAM 是唯一正解）。 */
static const struct device *const retained =
	DEVICE_DT_GET(DT_NODELABEL(d12retained));

static bool safe_halt;

bool entelea_platform_d12_safe_halt(void)
{
	return safe_halt;
}

static bool reset_is_abnormal(uint32_t *cause)
{
	if (hwinfo_get_reset_cause(cause) != 0) {
		return true; /* 读不出=保守按异常 */
	}
	hwinfo_clear_reset_cause();
	return (*cause & (RESET_WATCHDOG | RESET_SOFTWARE | RESET_POR)) !=
	       RESET_POR;
}

bool entelea_platform_d12_bootguard(void)
{
	uint32_t cause = 0;
	bool abnormal;

	static struct d12_rec guard;

	if (!device_is_ready(retained) ||
	    retained_mem_read(retained, 0, (uint8_t *)&guard, sizeof(guard)) != 0 ||
	    guard.magic != D12_MAGIC) {
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
	(void)retained_mem_write(retained, 0, (const uint8_t *)&guard, sizeof(guard));
	if (guard.count > D12_STORM_MAX) {
		safe_halt = true;
		printk("[d12] RESET-STORM SAFE-HALT: >=%u consecutive "
		       "abnormal resets; outputs at safe poweron, business "
		       "halted (power-cycle to exit)\n", D12_STORM_MAX);
	}
	return safe_halt;
}
