/* SPDX-License-Identifier: Apache-2.0 */
/* ts-net 初始化（boot 步骤接线，LLD-ts-net §2/§6）+ 周期驱动。
 * 周期体 = session_poll → linkmon_tick → pub_telem（依赖序）；遥测周期
 * 〔DEC-27：200ms〕为驱动节拍（session/linkmon 内部按各自周期判定）。
 * 板级七修正：周期体迁**专用工作队列**——zenoh open/close 为阻塞 TCP
 * 操作（死链上可达十余秒），在 sysworkq 上会饿死同队列工作（真机实证：
 * WiFi 重试工作迟 18s）；线程序遵循 DEC-43（net 优先级高于 APP=10）。 */
#include <string.h>
#include <ts/core.h>
#include <ts/net.h>
#include <ts/safety.h>
#include <ts/store.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "internal.h"

#ifdef CONFIG_TS_NET_ZENOH
extern const ts_net_transport_t ts_net_zenoh_transport;

static struct k_work_delayable net_work;
static bool started;

/* 专用工作队列（结构性数值：栈 8192 = zenoh 声明路径 + **DEC-49 tweetnacl
 * 验签深度**（远程安装路径 sys/app-verify 在本队列执行：crypto_sign_open
 * 局部 ~1KB + verify 管线 ~1.2KB——4096 实测被压穿〔dsdbench 真机
 * EXCCAUSE 28 教训〕）；优先级 8 = DEC-43 线程序 net > APP(10)）。 */
static K_KERNEL_STACK_DEFINE(net_wq_stack, 8192);
static struct k_work_q net_wq;

static void net_tick(struct k_work *work)
{
	struct k_work_delayable *d = k_work_delayable_from_work(work);
	uint64_t now = ts_time_ms();

	ts_wdt_feed(TS_WDT_NET); /* DEC-48②：net_wq 每轮喂狗（首次 = 激活） */
	(void)ts_net_session_poll(now);
	ts_net_linkmon_tick(now);
	ts_net_pub_telem(now);
	ts_net_pubq_flush(); /* 周期冲刷（CONNECTED 持续期；DOWN 期 pubq 自弃） */
	ts_net_avq_flush();  /* av 分片通道冲刷（DEC-47①②，MD1.2g；同语义） */
	k_work_reschedule_for_queue(&net_wq, d,
				    K_MSEC(CONFIG_TS_NET_TELEM_INTERVAL_MS));
}
#else
static bool started; /* 桩构建：ts_net_init 仅装配 ids/命令表/事件订阅 */
#endif

ts_res_t ts_net_init(void)
{
	if (started) {
		return TS_E_STATE;
	}
	started = true;

	/* ids ← prov（V1 单立方体 node = cube，DEC-26）。prov 缺失/损坏 → 开发
	 * 缺省（网络面保持 DOWN = 安全侧；生产板 boot 强校验随板级里程碑收紧）。 */
	const char *node = "n-dev";
	const char *cube = "c-dev";
	ts_res_t plr = ts_store_prov_load();
	const ts_prov_t *prov = ts_store_prov();

	if (plr == TS_OK && prov->cube_id[0] != '\0') {
		node = prov->node_id[0] != '\0' ? prov->node_id : prov->cube_id;
		cube = prov->cube_id;
	}
	printk("[ts-net] init prov_load=%d node=%s cube=%s\n", (int)plr, node, cube);
	(void)ts_net_set_ids(node, cube);

	/* CG2（Q-41① · LLD-ts-safety §3a）：standalone 部署 boot 声明链路——
	 * 镜像 TS_NET=n 构建的 glue BOOT_DONE 声明模式。输出自此 ACTIVE（值
	 * 仍为 poweron 安全值直至显式 commit——DR-04 无回写），本地合同全量
	 * 生效（estop/看门狗/限幅至高）；此后 linkmon 断链/恢复仅事件（§3a
	 * 零迁移）。orchestrated（含 v1 prov 与装载失败缺省）不声明——现行
	 * 为一分不变（boot 上下文线程态，set_link 写锁可用；通道表此刻已由
	 * 板级 main 注册完毕）。 */
	if (plr == TS_OK && prov->deploy_mode == TS_DEPLOY_STANDALONE) {
		ts_safety_set_link(true);
		printk("[ts-net] deploy_mode=standalone: link declared up (boot)\n");
	}

	/* sys 命令表 + 事件订阅（host_only；表满/订阅满 = 装配错误，如实返回） */
	ts_res_t r = ts_net_cmd_sys_init();

	if (r != TS_OK) {
		return r;
	}
	r = ts_net_pub_init();
	if (r != TS_OK) {
		return r;
	}
#ifdef CONFIG_TS_NET_ZENOH
	ts_net_set_transport(&ts_net_zenoh_transport);
	static bool wq_started;

	if (!wq_started) {
		k_work_queue_init(&net_wq);
		k_work_queue_start(&net_wq, net_wq_stack,
				   K_KERNEL_STACK_SIZEOF(net_wq_stack), 8, NULL);
		wq_started = true;
	}
	k_work_init_delayable(&net_work, net_tick);
	k_work_reschedule_for_queue(&net_wq, &net_work,
				    K_MSEC(CONFIG_TS_NET_TELEM_INTERVAL_MS));
#else
	/* 无传输（测试/桩构建）：不启动周期体——逻辑经测试直调驱动 */
#endif
	return TS_OK;
}
