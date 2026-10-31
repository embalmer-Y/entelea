/* SPDX-License-Identifier: Apache-2.0 */
/* linkdemo（单元 I / MD2-D9）：输入-逻辑-输出 + 断链时间线——D9 APP 滞回
 * 控制环（ADC→滞回→PWM）+ 强制断链 fail-safe（linkloss 值落驱动）+ 自愈
 * 恢复（DR-04：恢复不自动回写——APP 幂等重申即显式 commit）。
 * 流程（inputdemo 同型）：注册 dbpwm 通道 + ADC/PWM 真后端 → input monitor
 * 启动 → 安装 D9 包 → ts_core_boot 装载（步骤 8）→ 观测线程时间线：
 *   D9a link=ACTIVE →（APP armed 窗口：d9 out rc=0）→
 *   D9c link=DOWN → D9d duty_hw（want 0 = linkloss 落驱动）→
 *   D9e link=UP →（APP 重申成功：d9 block → d9 out recovered → D9-DONE）→
 *   D9f 终态 duty_hw。
 * 判据 = D9* 时间线行 + APP console 行（d9 out/d9 block/D9-DONE）——
 * 断链期 "d9 evt" 行继续出现 = 合同 3（输入流不因保护而中断）证据。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>
#include <ts/store.h>

#include "d9_pkg.h"

/* PWM 打包域（LLD-ts-hal §3）：高 16 = hz/100、低 16 = permille */
#define PWM_PACK(hz, pm) ((((hz) / 100U) << 16) | (pm))
static const ts_out_ch_t db_pwm = {
	.uid = "dbpwm", .kind = TS_CH_PWM,
	.poweron = {.u = PWM_PACK(1000, 0)},
	.linkloss = {.u = PWM_PACK(1000, 0)}, /* 断链态 = 0‰（D9d 判据值） */
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

/* esp32s3 LEDC 寄存器直读（perphbench/inputdemo 同型）+ 0% 特例判据：
 * Zephyr pwm_led_esp32 对 duty 0%（与 100%）走 STOP 路径——不更新 DUTY
 * 寄存器而是 SIG_OUT_EN=0 + IDLE_LV 输出（D9 板上首证）——故 duty_hw 判据
 * 仅在运行态有效；停机态判据 = SIG_OUT_EN==0 且 IDLE_LV==0（输出恒低）。 */
#define LEDC_BASE 0x60019000U
#define LEDC_REG(off) (*(volatile uint32_t *)(LEDC_BASE + (off)))
#define LEDC_LSCH0_CONF0   0x00U /* S3 位序（ledc_reg.h）：CONF0@0x0——
				   * HPOINT@0x4 / CONF1@0xC / DUTY_R@0x10 */
#define LEDC_LSCH0_DUTY_R  0x10U
#define LEDC_LSTIMER0_CONF 0xA0U
#define LEDC_SIG_OUT_EN    (1U << 2) /* S3 位序（ledc_reg.h）：SIG_OUT_EN=bit2 */
#define LEDC_IDLE_LV       (1U << 3) /* IDLE_LV=bit3（与初代 ESP32 位序不同） */

static uint32_t duty_permille_hw(void)
{
	uint32_t res = LEDC_REG(LEDC_LSTIMER0_CONF) & 0xFU;
	uint32_t duty = LEDC_REG(LEDC_LSCH0_DUTY_R) & 0x7FFFFU;

	if (res == 0U) {
		return 0xFFFFFFFFU; /* 未配置哨兵 */
	}
	return duty * 1000U / ((1U << res) * 16U);
}

static bool ledc_running(void)
{
	return (LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_SIG_OUT_EN) != 0U;
}

static void report_hw(const char *tag, uint32_t want)
{
	uint32_t conf0 = LEDC_REG(LEDC_LSCH0_CONF0);
	uint32_t hw = duty_permille_hw();

	if (!ledc_running()) {
		printk("%s stopped sig_en=0 idle_lv=%u (0%% 特例路径) %s\n", tag,
		       (conf0 & LEDC_IDLE_LV) ? 1U : 0U,
		       (want == 0U && (conf0 & LEDC_IDLE_LV) == 0U) ? "OK" : "MISMATCH");
		return;
	}
	printk("%s running duty_hw=%u‰ (want %u) %s\n", tag, hw, want,
	       (hw + 3U >= want && hw <= want + 3U) ? "OK" : "MISMATCH");
}

/* 时间线观测：armed 窗口 → 断链 → 自愈 → 终态（APP 行由 APP 打） */
static void demo_watch(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	ts_app_info_t info;

	for (int i = 0; i < 150; i++) {
		if (ts_appmgr_get_info(&info) == TS_OK &&
		    info.state == TS_APP_ACTIVE) {
			break;
		}
		k_msleep(200);
	}
	if (info.state != TS_APP_ACTIVE) {
		printk("LD FAIL no D9 ACTIVE\n");
		return;
	}
	printk("LD1 D9 ACTIVE app=%s\n", info.app_id);

	(void)ts_safety_set_link(true);
	printk("D9a link=ACTIVE（armed 窗口 10s——APP d9 out rc=0）\n");
	k_sleep(K_SECONDS(10));
	report_hw("D9b armed", 350U);

	/* 断链：输出进 linkloss 态（0‰ 落驱动——0% 特例路径）；输入事件照常
	 * 投递（合同 3——d9 evt 行继续）。 */
	(void)ts_safety_set_link(false);
	printk("D9c link=DOWN（fail-safe 3.5s）\n");
	k_sleep(K_MSEC(3500));
	report_hw("D9d linkloss", 0U);

	/* 自愈：恢复不自动回写（DR-04）——APP 幂等重申 = 显式 commit */
	(void)ts_safety_set_link(true);
	printk("D9e link=UP（恢复窗口 6s——APP d9 out recovered + D9-DONE）\n");
	k_sleep(K_SECONDS(6));
	report_hw("D9f recovered", 350U);

	for (int i = 0;; i++) {
		k_sleep(K_SECONDS(5));
		ts_appmgr_get_info(&info);
		printk("LD5 alive t=%u state=%d\n",
		       (unsigned)k_uptime_get_32(), (int)info.state);
	}
}
K_THREAD_DEFINE(demo_watch_tid, 8192, demo_watch, NULL, NULL, NULL, 13, 0, 0);

int main(void)
{
	printk("LD0 linkdemo（单元 I / MD2-D9：断链时间线）\n");

	ts_res_t lr = ts_store_prov_load();

	printk("LD0b prov load=%d（-8=空，缺省 ids）\n", (int)lr);

	if (ts_drv_pwm_init() != 0) {
		printk("LD FAIL pwm init\n");
		return 1;
	}
	if (ts_adc_drv_init() != 0) {
		printk("LD FAIL adc init\n");
		return 1;
	}
	(void)ts_safety_register_channel(&db_pwm);
	/* 注册序 = 实例号：ADC=0 / PWM=1（与 D9 caps adc:read:0/pwm:set:1 对齐） */
	(void)ts_hal_register_dev(&db_adc_dev);
	(void)ts_hal_register_dev(&db_pwm_dev);
	if (ts_hal_input_start() != TS_OK) {
		printk("LD FAIL input start\n");
		return 1;
	}
	printk("LD0c pwm+adc+input ready\n");

	ts_app_info_t info;

	if (ts_appmgr_install(d9_pkg, sizeof(d9_pkg), root_pub, &info) != TS_OK) {
		printk("LD FAIL D9 install\n");
		return 1;
	}
	printk("LD0d D9 installed slot=%u\n", info.active_slot);
	ts_core_boot(); /* noreturn：步骤 8 装载 D9 */
	return 0;
}
