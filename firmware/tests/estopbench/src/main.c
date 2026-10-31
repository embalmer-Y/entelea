/* SPDX-License-Identifier: Apache-2.0 */
/*
 * estopbench（板级八）：estop chosen DT 绑定（DR-11）真机验证
 * （docs/board-estop.md）。链路：GPIO9（BOOT 键脚）输入上升沿中断 →
 * estop_isr → ts_safety_force_all_fault（合同 5：直达，不经协议栈/调度
 * 队列）→ 通道 fault 值落驱动 + forced 锁存 + TS_EVT_ESTOP 事后补发
 * → ts_safety_clear_fault（host-only 语义同源，DEC-30①）恢复。
 *
 * 触发注入：无人按键条件下的自动化 = io_mux 双使能（bench 专用测试注入，
 * 与 boardbench 裸基线同类豁免——产品路径唯一写经保护层不变）：GPIO9 置
 * 输出使能并翻转输出寄存器 → 引脚电平真实变化 → 输入采样器 + GPIO 外设
 * 中断的完整硬件路径（非软件直调）。
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>

#define ESTOP_NODE DT_ALIAS(ts_estop_gpio)
BUILD_ASSERT(DT_NODE_EXISTS(ESTOP_NODE),
	     "estopbench board must provide alias ts-estop-gpio (overlay)");

static const struct gpio_dt_spec estop_key = GPIO_DT_SPEC_GET(ESTOP_NODE, gpios);

/* esp32s3 GPIO0-31 bank（TRM §5）：OUT_W1TS/W1TC、ENABLE_W1TS/W1TC */
#define GPIO0_BANK 0x60004000U
#define GPIO_REG(off) (*(volatile uint32_t *)(GPIO0_BANK + (off)))

static void inject_estop_rising_edge(void)
{
	uint32_t m = BIT(estop_key.pin);

	/* 输入保持使能（estop_key 已配输入+上拉），叠加输出使能 → 拉低 10ms
	 * → 拉高（上升沿 → estop_isr）→ 撤输出使能回纯输入。 */
	GPIO_REG(0x24) = m; /* ENABLE_W1TS */
	GPIO_REG(0x0C) = m; /* OUT_W1TC：低 */
	k_msleep(10);
	GPIO_REG(0x08) = m; /* OUT_W1TS：高 → 上升沿 */
	k_msleep(10);
	GPIO_REG(0x28) = m; /* ENABLE_W1TC */
}

static int estop_evt_seen;

static void on_estop_evt(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	estop_evt_seen++;
}

/* 通道 fault 值 = true（estop 后 readback.b 应为 true——poweron/linkloss 均
 * false，唯一置位来源 = fault 态直写，即 estop 链路证据） */
static const ts_out_ch_t eb_ch = {
	.uid = "eb0", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = true},
};
static const ts_hal_dev_desc_t eb_dev = {.uid = "eb0", .kind = TS_DEV_GPIO_OUT};

static bool eb_readback_is(bool want)
{
	ts_out_value_t v;

	return ts_safety_readback("eb0", &v) == TS_OK && v.b == want;
}

int main(void)
{
	printk("EB0 estopbench\n");

	/* EB1: estop 绑定（boot 步骤 1 同函数）+ 引脚输入上拉（悬空确定性） */
	if (ts_safety_estop_init() != TS_OK) {
		printk("EB FAIL estop_init\n");
		return 1;
	}
	if (gpio_pin_configure(estop_key.port, estop_key.pin,
			       GPIO_INPUT | GPIO_PULL_UP) != 0) {
		printk("EB FAIL pin cfg\n");
		return 1;
	}
	(void)ts_evt_subscribe(TS_EVT_ESTOP, on_estop_evt, NULL);

	/* EB2: 通道注册 + 链路声明（ACTIVE 前置）；poweron 态 readback=false */
	if (ts_hal_register_dev(&eb_dev) != TS_OK ||
	    ts_safety_register_channel(&eb_ch) != TS_OK) {
		printk("EB FAIL channel\n");
		return 1;
	}
	ts_safety_set_link(true);
	if (!eb_readback_is(false)) {
		printk("EB FAIL baseline readback\n");
		return 1;
	}
	printk("EB1 estop bound (GPIO%u) 通道基线 fault-free\n", estop_key.pin);

	/* EB3: 三轮 注入 → forced → 恢复 */
	for (int cycle = 1; cycle <= 3; cycle++) {
		uint32_t t0 = k_uptime_get_32();
		int ev_before = estop_evt_seen;

		inject_estop_rising_edge();
		/* forced 生效判定：通道态 SAFE_FAULT（ISR 直写 fault 值）+ readback */
		bool landed = false;
		ts_ch_state_t st;

		for (int i = 0; i < 100; i++) {
			if (ts_safety_channel_state("eb0", &st) == TS_OK &&
			    st == TS_ST_SAFE_FAULT && eb_readback_is(true)) {
				landed = true;
				break;
			}
			k_msleep(1);
		}
		uint32_t t1 = k_uptime_get_32();

		if (!landed) {
			printk("EB FAIL #%d estop not landed\n", cycle);
			return 1;
		}
		printk("EB3 #%d 硬件沿→fault 落通道≤%ums\n", cycle, t1 - t0);

		/* 事后补发（sysworkq 周期 → 本轮循环内以短等待兑现） */
		for (int i = 0; i < 50 && estop_evt_seen == ev_before; i++) {
			k_msleep(10);
		}
		/* 恢复：clear_fault（estop 锁存双检；DEC-30① 同源路径）。
		 * DR-04：恢复不自动回写 fault 前值——须显式 commit（安全侧语义）。 */
		if (ts_safety_clear_fault() != TS_OK) {
			printk("EB FAIL #%d clear_fault\n", cycle);
			return 1;
		}
		ts_safety_set_link(true);
		if (ts_safety_commit("eb0", (ts_out_value_t){.b = false}) != TS_OK ||
		    !eb_readback_is(false)) {
			printk("EB FAIL #%d recover readback\n", cycle);
			return 1;
		}
		printk("EB3 #%d estop_evt 补发=%d 恢复 fault-free\n",
		       cycle, estop_evt_seen - ev_before);
	}
	printk("EB PASS estop 硬件链路 ×3（沿触发/ISR 直达/锁存/补发/恢复）\n");
	for (;;) {
		k_msleep(5000);
		printk("EB alive t=%u\n", (unsigned)k_uptime_get_32());
	}
}
