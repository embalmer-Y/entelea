/* SPDX-License-Identifier: Apache-2.0 */
/* wdtbench（DEC-48 实施批）：看门狗接线真机验证——两阶段。
 * 阶段 B（L1/L2 防线）：装载 tick 死循环 APP（wdt_pkg.h 夹具，机械生成）
 * → 指令配额（TS_APP_INSTR_TICK）超限抛 "instruction limit exceeded"
 * → 宿主健康失败 3 连 → 回滚自停 + 系统存活（APPMGR 源持续喂狗）。
 * 阶段 C（L3 复位闭环）：sysworkq 协作级自旋工作项 → 巡检停喂 →
 * task_wdt 通道过期 → sys_reboot（串口观测重启即判据；该直通路径无
 * noinit 留痕——ISR 上下文不可写 flash，wdt.c 文件头如实登记）。
 * 判据（WB* console 行）：
 *   WB1 install（TEST 语义验签 = 结构检查）
 *   WB2 APP ACTIVE（boot 步骤 8 装载）
 *   WB3 回滚确认（state ≥ ROLLBACK + rollback_count ≥ 1 + 系统存活）
 *   WB PASS → 3s 后 WB4 阶段 C 自旋 → 预期复位（通道 5s） */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h>
#include <ts/core.h>

#include "wdt_pkg.h"

/* TEST 构建根公钥占位（pkg.c fail-closed：真验签未接入前非测试构建拒装） */
static const uint8_t root_pub[32];

static struct k_work spin_work;

static void phase_c_spin(struct k_work *w)
{
	ARG_UNUSED(w);
	for (;;) {
		/* 协作级自旋：饿死 sysworkq（巡检/estop 补发停摆）；ISR 与
		 * k_timer 不受影响 → task_wdt 通道过期 → sys_reboot。 */
	}
}

static void watcher_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	ts_app_info_t info;

	/* 等 APP 生命周期推进：tick 死循环 → 配额异常 ×3 → 回滚。
	 * 注：配额终止全程 ~600ms（3×[tick 周期 100ms + 配额上界]），快于
	 * 轮询周期——ACTIVE 瞬态可能错过；判据以终态为准（若配额失效，
	 * tick 永久挂起 → 永不回滚 → 超时 FAIL = 判别器）。 */
	bool rolled = false;
	int last_state = -1;

	for (int i = 0; i < 150; i++) {
		if (ts_appmgr_get_info(&info) == TS_OK) {
			if (info.state != last_state) {
				printk("WB2 state=%d rb=%u running=%d t=%ums\n",
				       (int)info.state, info.rollback_count,
				       (int)ts_appmgr_app_running(),
				       (unsigned)k_uptime_get_32());
				last_state = info.state;
			}
			if (info.state >= TS_APP_ROLLBACK &&
			    info.rollback_count >= 1) {
				rolled = true;
				break;
			}
		}
		k_msleep(100);
	}
	uint32_t t = (uint32_t)k_uptime_get_32();

	if (!rolled) {
		printk("WB FAIL no rollback (state=%d rb=%u running=%d)\n",
		       (int)info.state, info.rollback_count,
		       (int)ts_appmgr_app_running());
		return;
	}
	printk("WB3 rollback state=%d rb=%u running=%d（tick 死循环被配额终止）\n",
	       (int)info.state, info.rollback_count, (int)ts_appmgr_app_running());
	printk("WB PASS 阶段 B：L2 指令配额终止 + L1 系统存活 t=%ums\n", t);

	k_msleep(3000);
	printk("WB4 阶段 C：sysworkq 自旋 → 预期 task_wdt 复位（通道 %ums）\n",
	       CONFIG_TS_WDT_CHANNEL_MS);
	k_work_init(&spin_work, phase_c_spin);
	k_work_submit_to_queue(&k_sys_work_q, &spin_work);
	for (;;) {
		k_sleep(K_SECONDS(10));
		printk("WB alive t=%u（若见此行 = 阶段 C 复位失效）\n",
		       (unsigned)k_uptime_get_32());
	}
}
K_THREAD_DEFINE(wb_tid, 4096, watcher_thread, NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
	printk("WB0 wdtbench（DEC-48：看门狗接线真机验证）\n");

	ts_app_info_t info;
	ts_res_t r = ts_appmgr_install(wdt_pkg, sizeof(wdt_pkg), root_pub, &info);

	printk("WB1 install r=%d state=%d slot=%u app=%s\n",
	       (int)r, (int)info.state, info.active_slot, info.app_id);
	if (r != TS_OK) {
		printk("WB FAIL install\n");
		return 1;
	}
	ts_core_boot(); /* noreturn：步骤 8 装载 APP → 死循环 → 配额 → 回滚 */
	return 0;
}
