/* SPDX-License-Identifier: Apache-2.0 */
/* inputdemo（单元 F）：G3 + D4/D6 输入 demo 真机载体——ADC 真值 → input
 * monitor（事件）→ APP mailbox 路由全链 + D6 滞回控制环（PWM 限幅）。
 * 流程：注册 dbpwm 通道（限幅 700‰）→ ADC 后端 init → input monitor 启动
 * （G3 事件源）→ 安装 D4 包 → boot 装载 → 观测线程等 D4-DONE → stop →
 * 安装 D6 包（slot 翻转）→ boot 装载 → D6 滞回控制环 → 观测 D6-DONE。
 * 判据（ID* 行 + [app1/l*] 行，真值源 = GPIO2 悬空拾噪；确定性轨到轨
 * 注入 = owner 物理操作〔0V/3.3V，板级九 PB6 同型〕）。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include <ts/store.h>

#include "d4_pkg.h"
#include "d6_pkg.h"

/* PWM 打包域（LLD-ts-hal §3）：高 16 = hz/100、低 16 = permille */
#define PWM_PACK(hz, pm) ((((hz) / 100U) << 16) | (pm))
static const ts_out_ch_t db_pwm = {
	.uid = "dbpwm", .kind = TS_CH_PWM,
	/* poweron = 300‰（G1 判据取非零特异值——落驱动前 LEDC 复位缺省 0） */
	.poweron = {.u = PWM_PACK(1000, 300)},
	.linkloss = {.u = PWM_PACK(1000, 0)},
	.fault = {.u = PWM_PACK(1000, 0)},
	.limits = {.min = PWM_PACK(1000, 0), .max = PWM_PACK(1000, 700),
		   .slew_per_ms = 0},
};
static const ts_hal_dev_desc_t db_pwm_dev = {
	.uid = "dbpwm", .kind = TS_DEV_PWM,
};
static const ts_hal_dev_desc_t db_adc_dev = {
	.uid = "dbadc", .kind = TS_DEV_ADC,
};

static const uint8_t root_pub[32]; /* TEST 语义 = 固定测试根 */

/* esp32s3 LEDC 寄存器直读（periphbench 同型——单元 G1 真机判据：
 * boot 步骤 2 后、set_link 前物理 duty 已 = 声明 poweron 值）。 */
#define LEDC_BASE 0x60019000U
#define LEDC_REG(off) (*(volatile uint32_t *)(LEDC_BASE + (off)))
#define LEDC_LSCH0_DUTY_R 0x10U
#define LEDC_LSTIMER0_CONF 0xA0U

static uint32_t duty_permille_hw(void)
{
	uint32_t res = LEDC_REG(LEDC_LSTIMER0_CONF) & 0xFU;
	uint32_t duty = LEDC_REG(LEDC_LSCH0_DUTY_R) & 0x7FFFFU;

	if (res == 0U) {
		return 0xFFFFFFFFU; /* 未配置哨兵 */
	}
	return duty * 1000U / ((1U << res) * 16U);
}

/* 装载观测：D4 → stop → D6（进程内换包 = TEST 放行路径）→ 终态 */
static void demo_watch(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	ts_app_info_t info;

	/* 阶段 1：D4（等 ACTIVE + 存活窗口——报告行由 APP 打） */
	for (int i = 0; i < 150; i++) {
		if (ts_appmgr_get_info(&info) == TS_OK &&
		    info.state == TS_APP_ACTIVE) {
			break;
		}
		k_msleep(200);
	}
	if (info.state != TS_APP_ACTIVE) {
		printk("ID FAIL no D4 ACTIVE\n");
		return;
	}
	printk("ID1 D4 ACTIVE app=%s\n", info.app_id);
	/* G1 真机判据：set_link 前 LEDC 物理 duty 已 = 声明 poweron（300‰ ±3
	 * 容差 = periphbench 同口径；改前此处 = 0——落驱动缺口的直接证据） */
	k_msleep(10); /* 低数通道装载相位 */
	{
		uint32_t hw = duty_permille_hw();

		printk("ID1a poweron-duty hw=%u‰ (want 300) %s\n", hw,
		       (hw != 0xFFFFFFFFU && hw + 3 >= 300 && hw <= 303) ?
		       "OK" : "MISMATCH");
	}
	/* 无网面部署：链路 ACTIVE 由 bench 显式声明（persistbench 同型——
	 * 无 zenoh 则 linkmon 永不置位，输出写恒 E_STATE）。 */
	(void)ts_safety_set_link(true);
	printk("ID1b link=ACTIVE（输出写放行）\n");
	k_sleep(K_SECONDS(12)); /* 悬空拾噪驱动报告/事件行 */

	printk("ID2 stop D4 r=%d\n", (int)ts_appmgr_app_stop());

	/* 阶段 2：D6（slot 翻转 → boot 装载 → 滞回控制环） */
	if (ts_appmgr_install(d6_pkg, sizeof(d6_pkg), root_pub, &info) != TS_OK) {
		printk("ID FAIL D6 install\n");
		return;
	}
	printk("ID3 D6 installed slot=%u\n", info.active_slot);
	if (ts_appmgr_boot_start() != TS_OK) {
		printk("ID FAIL D6 boot\n");
		return;
	}
	for (int i = 0; i < 150; i++) {
		if (ts_appmgr_get_info(&info) == TS_OK &&
		    info.state == TS_APP_ACTIVE) {
			break;
		}
		k_msleep(200);
	}
	if (info.state != TS_APP_ACTIVE) {
		printk("ID FAIL no D6 ACTIVE\n");
		return;
	}
	printk("ID4 D6 ACTIVE app=%s\n", info.app_id);
	k_sleep(K_SECONDS(12));

	for (int i = 0;; i++) {
		k_sleep(K_SECONDS(5));
		ts_appmgr_get_info(&info);
		printk("ID5 alive t=%u state=%d\n",
		       (unsigned)k_uptime_get_32(), (int)info.state);
	}
}
K_THREAD_DEFINE(demo_watch_tid, 8192, demo_watch, NULL, NULL, NULL, 13, 0, 0);

int main(void)
{
	printk("ID0 inputdemo（单元 F：G3 + D4/D6）\n");

	/* prov 先烧（estop 沿 prov 化读取 + ids；缺省 dev ids 即可） */
	ts_res_t lr = ts_store_prov_load();

	printk("ID0b prov load=%d（-8=空，缺省 ids）\n", (int)lr);

	if (ts_drv_pwm_init() != 0) {
		printk("ID FAIL pwm init\n");
		return 1;
	}
	if (ts_adc_drv_init() != 0) {
		printk("ID FAIL adc init\n");
		return 1;
	}
	(void)ts_safety_register_channel(&db_pwm);
	/* 注册序 = 实例号：ADC=0 / PWM=1（与 demo caps adc:read:0/pwm:set:1 对齐） */
	(void)ts_hal_register_dev(&db_adc_dev);
	(void)ts_hal_register_dev(&db_pwm_dev);
	if (ts_hal_input_start() != TS_OK) { /* G3：事件源启动 */
		printk("ID FAIL input start\n");
		return 1;
	}
	printk("ID0c pwm+adc+input ready\n");

	ts_app_info_t info;

	if (ts_appmgr_install(d4_pkg, sizeof(d4_pkg), root_pub, &info) != TS_OK) {
		printk("ID FAIL D4 install\n");
		return 1;
	}
	printk("ID0d D4 installed slot=%u\n", info.active_slot);
	ts_core_boot(); /* noreturn：步骤 8 装载 D4 */
	return 0;
}
