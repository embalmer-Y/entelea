/* SPDX-License-Identifier: Apache-2.0 */
/* D12 平台启动防护（owner v1 保守方案 §9）。跨复位计数走 LP 域
 * retained RAM（r16 真机实证：__noinit 段变量在本板复位路径不可靠，
 * 风暴计数每次归零——见下 retained 注）。hwinfo 复位原因分类：
 * 非纯 POR（WDT/软件 fail-safe/panic/掉电/引脚）=异常；异常时①计
 * 风暴②通知 d12 60s 业务冷却窗。连续异常 ≥3（无 RTC 板：1 小时语义
 * 如实降级为连续计数，仅纯 POR 清零）→ 当次（POR 起第 4 次）启动
 * SAFE-HALT（输出安全起始态、不启业务、常驻喂狗、打印 [d12]
 * RESET-STORM——POR 出）。判定=entelea_d12_reset_guard（纯策略）。
 * 复位原因不可读或无法清除、留存设备不可用或读写失败均 fail-closed；
 * 仅纯 POR 可接受可读的未初始化留存记录。返回 true=SAFE-HALT。 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/retained_mem.h>
#include <entelea/d12.h>
#include "boot_steps.h"

#define D12_MAGIC 0x4431424DU /* "D12M" */

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

#ifdef ENTELEA_TEST
void
entelea_platform_d12_test_reset(void)
{
	safe_halt = false;
}
#endif

bool
entelea_platform_d12_safe_halt(void)
{
	return safe_halt;
}

static bool
reset_is_abnormal(uint32_t *cause, bool *known)
{
	int rc = hwinfo_get_reset_cause(cause);

	if (rc != 0) {
		printk("[d12] reset cause read failed: %d\n", rc);
		*known = false;
		return true; /* 读不出=保守按异常 */
	}
	rc = hwinfo_clear_reset_cause();
	if (rc != 0) {
		printk("[d12] reset cause clear failed: %d\n", rc);
		*known = false;
		return true;
	}
	*known = true;
	return *cause != RESET_POR;
}

bool
entelea_platform_d12_bootguard(void)
{
	struct d12_rec guard = { 0 };
	uint32_t cause = 0;
	bool cause_known;
	bool abnormal = reset_is_abnormal(&cause, &cause_known);
	bool ready = device_is_ready(retained);
	int read_rc = ready ? retained_mem_read(retained, 0, (uint8_t *) &guard,
						sizeof(guard)) :
			      -ENODEV;
	bool history_ok = read_rc == 0 && guard.magic == D12_MAGIC;
	bool halt;

	if (!history_ok) {
		guard.count = 0;
	}
	halt = entelea_d12_reset_guard(abnormal, history_ok, &guard.count);
	if (!cause_known || read_rc != 0) {
		if (read_rc != 0) {
			printk("[d12] retained read failed: %d\n", read_rc);
		}
		guard.count = ENTELEA_D12_RESET_STORM_MAX;
		halt = true;
	}
	guard.magic = D12_MAGIC;
	entelea_d12_boot_note(abnormal);
	if (abnormal) {
		printk("[d12] abnormal reset #%u cause=0x%x history=%s\n",
		       guard.count, cause, history_ok ? "ok" : "lost");
	}
	int write_rc = ready ? retained_mem_write(retained, 0,
						  (const uint8_t *) &guard,
						  sizeof(guard)) :
			       -ENODEV;

	if (write_rc != 0) {
		printk("[d12] retained write failed: %d\n", write_rc);
		halt = true;
	}
	if (halt) {
		safe_halt = true;
		printk("[d12] RESET-STORM SAFE-HALT: >=%u consecutive abnormal "
		       "resets or reset/retained state unknown; outputs at "
		       "safe "
		       "poweron, business halted (power-cycle to exit)\n",
		       ENTELEA_D12_RESET_STORM_MAX);
	}
	return safe_halt;
}
