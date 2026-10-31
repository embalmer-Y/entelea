/* SPDX-License-Identifier: Apache-2.0 */
/*
 * periphbench（板级九）：PWM/ADC 真后端真机验证（docs/board-periph.md）。
 *
 * PWM 输出链（合同 2 唯一写路径全链）：ts_pwm_set（权限裁决 + 打包域校验）
 * → ts_safety_commit（限幅→slew→审计）→ ts_drivers[TS_CH_PWM]（pwm_set →
 * LEDC）→ 硬件判据 = LEDC 寄存器直读：DUTY_R/DUTY_RES 比值 ↔ permille；
 * 0%/100% = 驱动停止态（CONF0.SIG_OUT_EN=0 + IDLE_LV 对应）；断链 fail-safe
 * = linkloss 声明值落驱动（HLD §4.5-S2，本批修复的真机首证）。
 * ADC 输入链（合同 3：输入直读不经保护层）：io_mux 输出使能注入（estopbench
 * 同型豁免——引脚电平真实 0V/3.3V）→ ts_adc_read（eFuse 校准 mV）轨判据。
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/periph.h>
#include <ts/safety.h>

/* esp32s3 LEDC（TRM §19；偏移自 hal_espressif esp32s3 ledc_reg.h）：
 * LSCH0 寄存器组 + LSTIMER0_CONF（duty_res [3:0]）。 */
#define LEDC_BASE 0x60019000U
#define LEDC_REG(off) (*(volatile uint32_t *)(LEDC_BASE + (off)))
#define LEDC_LSCH0_CONF0 0x00U
#define LEDC_LSCH0_DUTY_R 0x10U
#define LEDC_LSTIMER0_CONF 0xA0U
#define LEDC_SIG_OUT_EN BIT(2)
#define LEDC_IDLE_LV BIT(3)

/* esp32s3 GPIO0-31 bank：ADC 轨注入（estopbench 同型） */
#define GPIO0_BANK 0x60004000U
#define GPIO_REG(off) (*(volatile uint32_t *)(GPIO0_BANK + (off)))
#define ADC_PIN 2 /* D1 = ADC1_CH1 */
#define ADC_MASK BIT(ADC_PIN)

/* PWM 打包域（LLD-ts-hal §3）：高 16 = hz/100、低 16 = permille */
#define PWM_PACK(hz, pm) ((((hz) / 100U) << 16) | (pm))

/* duty 比值容差：驱动 duty_val = 2^res × ratio 截断 + DUTY_R 更新相位，
 * res≥13 时粒度 <0.2‰——3‰ 覆盖舍入与更新滞后 */
#define DUTY_TOL_PM 3U

static bool bench_fail;

static void check(bool ok, const char *tag)
{
	if (!ok) {
		bench_fail = true;
		printk("PP CHECK-FAIL %s\n", tag);
	}
}

/* 描述符（生产同型：经 ts_periph_register 全链注册）。三安全态 = 0%（LED 灭，
 * 安全侧）；限幅 max = 5000Hz|700‰（PP3 clamp 判据，hz 与扫描上限一致——
 * 打包域比较 hz 占高位，clamp 请求须取 max 同 hz 才是纯 duty 判据）。 */
static const ts_periph_desc_t pwm_desc = {
	.uid = "pb-pwm", .kind = TS_PK_PWM,
	.safe = {.uid = "pb-pwm", .kind = TS_CH_PWM,
		 .poweron = {.u = PWM_PACK(1000, 0)},
		 .linkloss = {.u = PWM_PACK(1000, 0)},
		 .fault = {.u = PWM_PACK(1000, 0)},
		 .limits = {.min = PWM_PACK(1000, 0),
			    .max = PWM_PACK(5000, 700),
			    .slew_per_ms = 0}},
};
static const ts_periph_desc_t adc_desc = {
	.uid = "pb-adc", .kind = TS_PK_ADC,
};

/* LEDC 实测 duty（permille 域）：DUTY_R / (2^duty_res × 16)。寄存器字段 =
 * duty ticks << 4（hal ledc_ll_set_duty_int_part：hw->duty = duty_val << 4，
 * 有效位 [18:4]）；res=0 = 未配置哨兵 */
static uint32_t duty_permille_hw(void)
{
	uint32_t res = LEDC_REG(LEDC_LSTIMER0_CONF) & 0xFU;
	uint32_t duty = LEDC_REG(LEDC_LSCH0_DUTY_R) & 0x7FFFFU;

	if (res == 0U) {
		return 0xFFFFFFFFU;
	}
	return duty * 1000U / ((1U << res) * 16U);
}

static bool duty_matches(uint32_t want_pm)
{
	uint32_t hw = duty_permille_hw();

	return hw != 0xFFFFFFFFU && hw + DUTY_TOL_PM >= want_pm &&
	       hw <= want_pm + DUTY_TOL_PM;
}

/* duty 更新相位落定（低数通道下一周期装载，1kHz = 1ms） */
static void ledc_settle(void)
{
	k_msleep(6);
}

static void adc_drive_low(void)
{
	GPIO_REG(0x24) = ADC_MASK; /* ENABLE_W1TS */
	GPIO_REG(0x0C) = ADC_MASK; /* OUT_W1TC：0V */
	k_msleep(5);
}

static void adc_drive_high(void)
{
	GPIO_REG(0x24) = ADC_MASK; /* ENABLE_W1TS */
	GPIO_REG(0x08) = ADC_MASK; /* OUT_W1TS：3.3V */
	k_msleep(5);
}

static void adc_release(void)
{
	GPIO_REG(0x28) = ADC_MASK; /* ENABLE_W1TC：回悬空 */
}

int main(void)
{
	printk("PP0 periphbench\n");

	/* PP1: 真后端 init + 描述符全链注册（ts-periph→ts-safety→ts-hal） */
	if (ts_drv_pwm_init() != 0) {
		printk("PP FAIL pwm_init\n");
		return 1;
	}
	if (ts_adc_drv_init() != 0) {
		printk("PP FAIL adc_init\n");
		return 1;
	}
	if (ts_periph_register(&pwm_desc) != TS_OK ||
	    ts_periph_register(&adc_desc) != TS_OK) {
		printk("PP FAIL register\n");
		return 1;
	}
	printk("PP1 backends+descriptors ok\n");

	/* 权限上下文：pwm=set 实例 0（首注册）；adc=read 实例 1（次注册） */
	ts_perm_table_t table;
	ts_ctx_t ctx;

	ts_perm_table_init(&table);
	if (ts_perm_parse("pwm:set:0", &table) != TS_OK ||
	    ts_perm_parse("adc:read:1", &table) != TS_OK ||
	    ts_hal_bind_context(&ctx, 1, &table) != TS_OK) {
		printk("PP FAIL perm bind\n");
		return 1;
	}

	/* 上电态 = 0% → 驱动停止态（SIG_OUT_EN=0；timer 未配置，无比值判据） */
	check((LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_SIG_OUT_EN) == 0,
	      "poweron-stop");

	/* PP2: duty 扫描（ACTIVE 态经全保护链；含跨 hz 重配 1000→5000） */
	ts_safety_set_link(true);
	static const struct {
		uint32_t hz, pm;
	} sweep[] = {
		{1000, 100}, {1000, 250}, {1000, 500},
		{1000, 700}, {5000, 500}, {1000, 400},
	};

	for (size_t i = 0; i < ARRAY_SIZE(sweep); i++) {
		ts_res_t r = ts_pwm_set(ctx, 0, sweep[i].hz, sweep[i].pm);

		ledc_settle();
		uint32_t hw = duty_permille_hw();

		check(r == TS_OK, "sweep-commit");
		check(duty_matches(sweep[i].pm), "sweep-duty");
		check((LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_SIG_OUT_EN) != 0,
		      "sweep-run");
		printk("PP2 hz=%u pm=%u hw_pm=%u duty_res=%u r=%d\n",
		       sweep[i].hz, sweep[i].pm, hw,
		       LEDC_REG(LEDC_LSTIMER0_CONF) & 0xFU, (int)r);
	}

	/* PP3: 保护层限幅落硬件——900‰ 请求 → 700‰ 落 LEDC + TS_E_RANGE */
	ts_res_t r = ts_pwm_set(ctx, 0, 5000, 900);

	ledc_settle();
	check(r == TS_E_RANGE, "clamp-range");
	check(duty_matches(700), "clamp-duty");
	printk("PP3 clamp 900->700 hw_pm=%u r=%d\n", duty_permille_hw(), (int)r);

	/* PP4: 端点 0%/100% = 驱动停止态语义（SIG_OUT_EN=0 + IDLE_LV 对应） */
	r = ts_pwm_set(ctx, 0, 1000, 1000);
	ledc_settle();
	check(r == TS_OK, "endpoint-1000");
	check((LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_SIG_OUT_EN) == 0,
	      "endpoint-1000-stop");
	check((LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_IDLE_LV) != 0,
	      "endpoint-1000-idle-high");
	r = ts_pwm_set(ctx, 0, 1000, 0);
	ledc_settle();
	check(r == TS_OK, "endpoint-0");
	check((LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_SIG_OUT_EN) == 0,
	      "endpoint-0-stop");
	check((LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_IDLE_LV) == 0,
	      "endpoint-0-idle-low");
	printk("PP4 endpoints ok r=%d\n", (int)r);

	/* PP5: 断链 fail-safe——linkloss 声明值（0%）落驱动（HLD §4.5-S2），
	 * 恢复后显式重设可写回（DR-04：不自动回写） */
	ts_pwm_set(ctx, 0, 1000, 600); /* 断链前值 600‰ */
	ledc_settle();
	check(duty_matches(600), "pre-linkloss-600");
	ts_safety_set_link(false);
	ledc_settle();
	check((LEDC_REG(LEDC_LSCH0_CONF0) & LEDC_SIG_OUT_EN) == 0,
	      "linkloss-stop");
	r = ts_pwm_set(ctx, 0, 1000, 500);
	check(r == TS_E_STATE, "linkloss-write-reject");
	ts_safety_set_link(true);
	r = ts_pwm_set(ctx, 0, 1000, 400);
	ledc_settle();
	check(r == TS_OK && duty_matches(400), "recover-writeback");
	printk("PP5 linkloss fail-safe + recover ok (r=%d hw_pm=%u)\n",
	       (int)r, duty_permille_hw());

	/* PP6/7: ADC 轨到轨（真实电平注入 → eFuse 校准 mV 判据；
	 * 12dB 衰减满量程 < 3.3V——高轨 = 饱和读数，mv 判下界即可） */
	int32_t mv = -1;

	adc_drive_low();
	r = ts_adc_read(ctx, 1, &mv);
	check(r == TS_OK && mv < 150, "adc-low-rail");
	printk("PP6 adc low rail mv=%d r=%d\n", mv, (int)r);

	adc_drive_high();
	r = ts_adc_read(ctx, 1, &mv);
	check(r == TS_OK && mv > 2000, "adc-high-rail");
	printk("PP7 adc high rail mv=%d r=%d\n", mv, (int)r);

	adc_release();
	/* 悬空参考读（无判据，观测记录） */
	r = ts_adc_read(ctx, 1, &mv);
	printk("PP8 adc floating mv=%d r=%d（观测）\n", mv, (int)r);

	/* PWM 写失败计数 = 0（后端全链零静默失败） */
	check(ts_drv_pwm_err_count() == 0, "pwm-err-count");

	if (bench_fail) {
		printk("PP FAIL\n");
		return 1;
	}
	printk("PP PASS\n");
	return 0;
}
