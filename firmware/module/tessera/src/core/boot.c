/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 初始化编排器（LLD-ts-core §2）：顺序表驱动；合同 6（任一步失败 → 全系统 fail-safe）。
 */
#include <ts/core.h>
#include <ts/safety.h>
#include <zephyr/kernel.h>
#ifdef CONFIG_TS_STORE_AUDIT
#include <ts/store.h>
#endif

#if defined(CONFIG_TS_NET)
#include <ts/net.h>
#endif

#if defined(CONFIG_TS_APP_WAMR)
#include <ts/appmgr.h>
#include <zephyr/sys/printk.h>
#endif

/* 事件 payload 类型（core.h 侧集中定义于本文件上方 include；此处仅实现） */

static ts_res_t step_estop_gpio(void)
{
	/* estop DT 绑定（DR-11）：chosen 节点 ts,estop-gpio；无节点板（native_sim/测试）空操作。
	 * 生产板必须提供（板级断言 M2+ 强化）。实现在 ts-safety force.c。 */
	return ts_safety_estop_init();
}

static ts_res_t step_safety_poweron(void)
{
	/* 全通道置 SAFE_POWERON 初态（注册表此刻通常为空——通道随 ts-periph 注册；
	 * 本步确保任何“boot 期已注册”通道即刻进入安全态）。 */
	return ts_safety_poweron_init();
}

static ts_res_t step_wdt_start(void)
{
	return ts_wdt_start();
}

static ts_res_t step_core_init(void)
{
	/* 事件/时间服务为静态初始化（无动态 init）；本步保留为编排占位以固定顺序，
	 * M2 起 periph/hal/store/net/appmgr 步骤按 HLD §4.4 顺序尾部追加。 */
	return TS_OK;
}

#if defined(CONFIG_TS_NET)
static ts_res_t step_net_init(void)
{
	/* ts-net 接线（M3a.2）：ids/pub/命令表 + （CONFIG_TS_NET_ZENOH）传输绑定
	 * 与周期驱动。prov 缺失 → 开发缺省 ids 且网络面保持 DOWN（安全侧）；
	 * 事件订阅满员（装配错误）→ 如实失败（合同 6 fail-safe）。 */
	return ts_net_init();
}
#endif

#if defined(CONFIG_TS_APP_WAMR)
static ts_res_t step_app_load(void)
{
	/* 步骤 8（HLD §4.4-8，M2b.2 收尾）：从 active slot 装载 APP。
	 * APP 级故障**不阻塞系统启动**（HLD §4.4-8 = 合同 6 显式例外）——
	 * 失败仅 printk 留痕并返回 OK。 */
	ts_res_t r = ts_appmgr_boot_start();

	if (r != TS_OK) {
		printk("[boot] step8 app load: 无 APP 或装载跳过（r=%d，不阻塞启动）\n",
		       (int)r);
	}
	return TS_OK;
}
#endif

/*
 * 顺序 = HLD §4.4（规格，不可调换；只允许尾部追加）：
 * estop GPIO → 全通道 SAFE_POWERON → WDT 启动 → core → net（M3a.2 追加）
 * → app load（M2b.2 追加，步骤 8）→ audit 恢复（V2-2/DEC-52① 追加）。
 */
#if defined(CONFIG_TS_STORE_AUDIT)
static ts_res_t step_audit_init(void)
{
	/* DEC-52①：flash 审计环游标恢复 + boot 报告（最近 8 条人读留痕）。
	 * init 失败不阻塞启动（观测面降级——内存环照常工作）。 */
	ts_res_t r = ts_store_audit_init();

	if (r != TS_OK) {
		printk("[boot] audit_init 失败 r=%d（flash 审计降级）\n", (int)r);
		return TS_OK;
	}
	ts_store_audit_boot_report();
	return TS_OK;
}
#endif

const ts_boot_step_t ts_boot_steps[TS_BOOT_STEP_COUNT] = {
	{"estop_gpio", step_estop_gpio},
	{"safety_poweron", step_safety_poweron},
	{"wdt_start", step_wdt_start},
	{"core_init", step_core_init},
#if defined(CONFIG_TS_NET)
	{"net_init", step_net_init},
#endif
#if defined(CONFIG_TS_APP_WAMR)
	{"app_load", step_app_load},
#endif
#if defined(CONFIG_TS_STORE_AUDIT)
	{"audit_init", step_audit_init},
#endif
};

FUNC_NORETURN void ts_core_boot(void)
{
	for (int i = 0; i < TS_BOOT_STEP_COUNT; i++) {
		ts_res_t res = ts_boot_steps[i].fn();
		/* 每步完成后发布 BOOT_STEP（含失败步的 result）——重放观测点；
		 * “执行前后发布”收敛为完成后单条（失败步亦有观测），语义等价且 golden 稳定。 */
		const ts_boot_step_evt_t payload = {
			.idx = (uint8_t)i,
			.result = res,
			.name = ts_boot_steps[i].name,
		};
		const ts_evt_t evt = {
			.id = TS_EVT_BOOT_STEP,
			.t_ms = ts_time_ms(),
			.data = &payload,
			.len = sizeof(payload),
		};

		ts_evt_publish(&evt);
		if (res != TS_OK) {
			/* 合同 6：任一步失败 → 全系统 fail-safe，不得半启动。
			 * noinit 留痕经 ts-store（M2 接入，当前为弱桩）。 */
			ts_safety_system_fail(TS_FAIL_BOOT(i));
			for (;;) {
				k_sleep(K_FOREVER); /* 停喂硬 WDT → 兜底复位 */
			}
		}
	}

	const ts_evt_t done = {
		.id = TS_EVT_BOOT_DONE,
		.t_ms = ts_time_ms(),
	};
	ts_evt_publish(&done);

	/* main 线程 init 后转监督（LLD-ts-common §4 线程表） */
	for (;;) {
		k_sleep(K_FOREVER);
	}
}
