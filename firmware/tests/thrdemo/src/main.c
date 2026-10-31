/* SPDX-License-Identifier: Apache-2.0 */
/* thrdemo（V2 纵深批 M19b，DEC-55/Q-37 A）：D-THR demo 真机载体——
 * 真 wasm 多线程全链。流程：threads=2 注入 → ts_appmgr_app_start（wasm
 * 直嵌——tests/app 同款 RAM 路径，无 store）→ APP 状态机：主线程
 * ts_thread_start("count_worker", 2000) → worker 共享内存原子累加 →
 * ts_thread_join → 校验 → "DT DONE cnt=2000 rc=2000"。
 * 判据（console DT* 行）：DT0 启动 → DT1 启动成功 → DT2 APP ACTIVE 存活
 * 观测（state/health）→ APP 侧 "dtr init"/"DT DONE ..." → **DT PASS**。 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h>
#include <ts/core.h>

#include "dt_wasm.h"

int main(void)
{
	printk("DT0 thrdemo（V2 纵深 M19b：D-THR 真 wasm 多线程全链）\n");
	/* DEC-55：manifest threads 申报（boot 装载路径之外的显式注入） */
	ts_appmgr_app_set_threads(1, 2);
	ts_res_t r = ts_appmgr_app_start(1, dt_wasm, sizeof(dt_wasm), "");

	printk("DT1 app_start r=%d\n", (int)r);
	if (r != TS_OK) {
		printk("DT FAIL start\n");
		return 1;
	}
	for (int i = 0;; i++) {
		k_msleep(5000);
		struct ts_app_rt_stats st;

		ts_appmgr_app_stats(&st);
		printk("DT2 alive t=%u state=%d ticks=%u health_fails=%u\n",
		       (unsigned)k_uptime_get_32(), (int)st.running,
		       st.tick_count, st.health_fails);
	}
	return 0;
}
