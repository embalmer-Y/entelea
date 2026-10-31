/* SPDX-License-Identifier: Apache-2.0 */
/* ts-drv ES8311 音频输入输出后端（K2 · Q-44/DEC-61）：I2S 标准模式 + I2C 控制
 * 的全 codec（ADC 录音 + DAC 放音）——ATK DNESP32-S3B V1.1 实机面（真机
 * AK AUDIO PASS：录音 8 帧非静默 + 放音链 + SPK_EN）。
 *
 * 硬件（owner 原理图/引脚表核验，2026-10-15）：
 *   I2S0（标准 Philips，主机）：BCK=IO21、WS/LRCK=IO13、DSDIN(出)=IO14、
 *   ASDOUT(入)=IO47；无独立 MCLK——**ES8311 从 SCLK 内部取源（原理图注记
 *   「SLCK 必须>1Mhz」）** ⇒ 32-bit 槽使 BCLK = 16k×64 = 1.024MHz > 1MHz。
 *   I2C0（SCL=IO45/SDA=IO48，与触摸/XL9555 共线）@0x18（CE=0）。
 *   功放 NS4168 使能 SPK_EN 在 XL9555 port1 bit3（裸 I2C 驱动——atkbench
 *   overlay 禁用 pca9555 节点防驱动缓存互踩）。
 *
 * 时钟（fs=16kHz mono，帧 = 320 样本；av pcm16 语义零变化）：
 *   XTAL 40M → mclk frac(4+226/256) = 8.192MHz → bck_div 8 → BCLK 1.024M；
 *   WS = BCLK/64 = 16k。32-bit 槽内 16-bit 样本（MSB 对齐：DMA 帧 1280B，
 *   拷贝时 <<16 / >>16 转换）。codec 时钟行 = 官方系数表 {mclk=1.024M,
 *   rate=16k}（esp-bsp components/es8311——Apache-2.0 参考；新表 pre_multi
 *   为档位索引：×4 → reg02 bit[4:3]=2）。
 *
 * bring-up 六坑定案（2026-10-15/16 真机逐坑实证）：
 *   ① TX 描述符 dw0.length 必填（只设 size = 0 字节传输 → FIFO 永不填 →
 *      BCK/WS 引脚全平；RX 无此语义——pdm.c 先例误导）；
 *   ② 全双工共享总线须 i2s_ll_share_bck_ws（tx_conf.sig_loopback——TX 时钟
 *      环回进 RX；树内 i2s_esp32 驱动同款关键调用）；
 *   ③ GPIO 输出态读引脚须 pad_input_enable（否则 IN 寄存器假读恒 0——
 *      诊断数据自证不可信即停）；
 *   ④ 时钟域配置序对齐 hal（enable_clock 先于 bind/src/mclk/bck；RX 先配
 *      TX 后配——mclk_sel 终态 = tx）；
 *   ⑤ 影子寄存器 update 锁存（停止态配置生效前置）；
 *   ⑥ **REG00 bit7 上电位恒保持**（esp-bsp 2026 现代复位序 0x1F→20ms→
 *      0x00→0x80——旧 ADF 序的 0x00 会清掉上电位 = codec 从未上电、ASDOUT
 *      静默；本坑为 ADC 静默真根因）。
 * 数据面：RX = GDMA ch0 环形 4×1280B（EOF 轮询取帧——pdm.c 同型）；TX =
 * ch1 静音环常开（4×1280B 零填充永续 DMA——承载 BCK/WS 连续性）；写入 =
 * 环内前方槽打补丁（EOF 进度 + 实时限流）。
 * 寄存器语义出处：ES8311 公开数据手册 + esp-bsp components/es8311（Apache-
 * 2.0）init/系数/启停序列对照；域内自研最小面（零上游补丁纪律）。 */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/cache.h>
#include <hal/i2s_ll.h>
#include <hal/gdma_ll.h>
#include <hal/dma_types.h>
#include <hal/hal_utils.h>
#include <soc/gpio_sig_map.h>
#include <rom/gpio.h>
#include <ts/core.h>
#include <ts/hal.h>

/* 引脚绑定：zephyr,user（bck/ws/sd-out/sd-in 四 gpios——pdm.c 同族 DT 纪律）*/
#if !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), es8311_bck_gpios) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), es8311_ws_gpios) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), es8311_sdout_gpios) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), es8311_sdin_gpios)
#error "TS_DRV_ES8311=y：zephyr,user 缺 es8311-{bck,ws,sdout,sdin}-gpios"
#endif
static const struct gpio_dt_spec pin_bck =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), es8311_bck_gpios);
static const struct gpio_dt_spec pin_ws =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), es8311_ws_gpios);
static const struct gpio_dt_spec pin_sdout =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), es8311_sdout_gpios);
static const struct gpio_dt_spec pin_sdin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), es8311_sdin_gpios);

#define ES8311_I2C_NODE DT_NODELABEL(i2c0)
#if !DT_NODE_HAS_STATUS(ES8311_I2C_NODE, okay)
#error "TS_DRV_ES8311=y：i2c0 节点未使能"
#endif
static const struct device *const es_i2c = DEVICE_DT_GET(ES8311_I2C_NODE);
#define ES8311_ADDR 0x18 /* 7 位（CE=0——原理图核验） */

#define ES_FS_HZ        16000U
#define ES_SLOT_BYTES   4U                       /* 32-bit 槽 */
#define ES_SAMPLES      320U                     /* 帧 = 320 样本（640B pcm16） */
#define ES_FRAME_DMA    (ES_SAMPLES * ES_SLOT_BYTES) /* 1280B */
#define ES_FRAME_PCM16  (ES_SAMPLES * 2U)        /* 640B */
#define ES_BCLK_HZ      1024000U                 /* 16k×64——原理图 >1MHz 约束 */
#define ES_BCK_DIV      8U                       /* mclk 8.192M / BCLK 1.024M */
#define GDMA_CH_RX      0U
#define GDMA_CH_TX      1U
#define GDMA_PERIPH_I2S0 3                       /* dma_esp32.h（pdm.c 同值） */
#define ES_RING_N       4U

static i2s_dev_t *const i2s = &I2S0;
static gdma_dev_t *const gdma = &GDMA;

static dma_descriptor_align4_t es_rx_desc[ES_RING_N] __aligned(4);
static uint8_t es_rx_buf[ES_RING_N][ES_FRAME_DMA] __aligned(4);
static uint32_t es_rx_last_eof;
static dma_descriptor_align4_t es_tx_desc[ES_RING_N] __aligned(4);
static uint8_t es_tx_buf[ES_RING_N][ES_FRAME_DMA] __aligned(4);
static uint32_t es_tx_written; /* 已写入槽数（消费进度 = TX EOF 描述符）*/
static bool es_inited;

/* ---- I2C 寄存器面 --------------------------------------------------------- */

static int es8311_wr(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};

	return i2c_write(es_i2c, buf, sizeof(buf), ES8311_ADDR);
}

static int es8311_rd(uint8_t reg, uint8_t *val)
{
	return i2c_write_read(es_i2c, ES8311_ADDR, &reg, 1, val, 1);
}

/* XL9555 输出（SPK_EN）：裸 I2C（pca9555 驱动节点在载体 overlay 禁用）。
 * port1 配置/输出寄存器 = 0x07/0x03（默认输入态——置输出前先写配置位）。
 * SPK_EN = port1 bit3（V1.1 原理图——K2 实证域）。 */
#define XL9555_ADDR   0x20
#define XL9555_P1_OUT 0x03
#define XL9555_P1_CFG 0x07
static int es8311_spk_en(bool on)
{
	uint8_t cfg = 0, out = 0;

	if (es8311_rd(XL9555_P1_CFG, &cfg) != 0) {
		return -1;
	}
	if (es8311_rd(XL9555_P1_OUT, &out) != 0) {
		return -1;
	}
	cfg &= (uint8_t)~BIT(3); /* bit3 输出 */
	/* 极性：原理图网络名 NLSPK0EN——N 前缀 = **低有效**（bit3=0 = 功放开、
	 * 1 = 关断）。初版极性反了：spk(false) 落 0 = 功放常开放大 DAC 静态
	 * 噪声（真机可听——owner 反馈，K2 实证第七坑）。 */
	out = on ? (uint8_t)(out & ~BIT(3)) : (uint8_t)(out | BIT(3));
	if (es8311_wr(XL9555_P1_CFG, cfg) != 0 ||
	    es8311_wr(XL9555_P1_OUT, out) != 0) {
		return -1;
	}
	return 0;
}

/* 静默/无帧诊断（pdm.c 诊断同型——引脚电平采样 + codec 寄存器回读；
 * 仅失败路径调用，正常路径零开销。 */
static void es8311_silence_diag(void)
{
	uint32_t bck = 0, ws = 0, sdi = 0;
	const uint32_t samples = 20000;

	/* 输入缓冲使能（输出态 pad 读 IN 寄存器须开 input enable——否则恒 0
	 * 的假读数；K2 实证第三坑） */
	rom_gpio_pad_input_enable(pin_bck.pin);
	rom_gpio_pad_input_enable(pin_ws.pin);
	for (uint32_t i = 0; i < samples; i++) {
		bck += gpio_pin_get_raw(pin_bck.port, pin_bck.pin);
		ws += gpio_pin_get_raw(pin_ws.port, pin_ws.pin);
		sdi += gpio_pin_get_raw(pin_sdin.port, pin_sdin.pin);
	}
	uint8_t r01 = 0, r02 = 0, r17 = 0;

	es8311_rd(0x01, &r01);
	es8311_rd(0x02, &r02);
	es8311_rd(0x17, &r17);
	printk("[es8311] diag: bck=%u/100 ws=%u/100 sdi=%u/100 r01=%02x r02=%02x r17=%02x\n",
	       (unsigned)(bck * 100 / samples), (unsigned)(ws * 100 / samples),
	       (unsigned)(sdi * 100 / samples), r01, r02, r17);
	printk("[es8311] regs: tx_conf=%08x tx_clkm=%08x tx_div=%08x conf1=%08x\n",
	       (unsigned)i2s->tx_conf.val, (unsigned)i2s->tx_clkm_conf.val,
	       (unsigned)i2s->tx_clkm_div_conf.val, (unsigned)i2s->tx_conf1.val);
}

/* 上电初始化（esp-bsp 2026 现代序 + 系数行 {1.024M,16k} + ADC/DAC 启动；
 * 从机模式——S3 主机出 BCK/WS）。返回 0 = OK。 */
int ts_drv_es8311_init(void)
{
	if (es_inited) {
		return 0;
	}
	if (!device_is_ready(es_i2c)) {
		printk("[es8311] i2c0 not ready\n");
		return -1;
	}
	/* 芯片在位探测（0x00 读）——无芯片 = 如实失败（板未配 ES8311） */
	uint8_t dummy;

	if (es8311_rd(0x00, &dummy) != 0) {
		printk("[es8311] no chip @0x%02x\n", ES8311_ADDR);
		return -2;
	}

	/* 复位序列（esp-bsp 2026 现代序）：0x1F 全复位 → 20ms 稳定 → 0x00 释放
	 * → 0x80 上电（**bit7 恒保持**——清掉上电位 = codec 不上电、ASDOUT
	 * 静默，K2 实证第六坑/真根因）；bit6=0 = 从机（S3 主机） */
	int bad = 0;

	bad |= es8311_wr(0x00, 0x1F);
	k_msleep(20);
	bad |= es8311_wr(0x00, 0x00);
	bad |= es8311_wr(0x00, 0x80);

	/* 时钟：0x01 = 0x3F（全时钟使能）| bit7（MCLK 源 = SCLK——无 MCLK 引脚
	 * 板型）→ 0xBF；系数行 {mclk=1.024M, rate=16k}：
	 * pre_div=1/pre_multi=×4（reg02 bit[4:3]=2）/adc_div=1/dac_div=1/
	 * fs=ss/lrck=0x00,0xff/bclk_div=4/osr=0x10,0x10 */
	bad |= es8311_wr(0x01, 0xBF);
	bad |= es8311_wr(0x02, (0U << 5) | (2U << 3));
	bad |= es8311_wr(0x03, (0U << 6) | 0x10);
	bad |= es8311_wr(0x04, 0x10);
	bad |= es8311_wr(0x05, (0U << 4) | (0U << 0));
	bad |= es8311_wr(0x06, 0x03); /* bit5=0 不反相 + bclk_div=4 低 5 位 */
	bad |= es8311_wr(0x07, 0x00);
	bad |= es8311_wr(0x08, 0xFF);

	/* SDP 格式：I2S Philips（fmt=00）+ 32-bit 字（4<<2 即 0x10） */
	bad |= es8311_wr(0x09, 0x10); /* DAC 串行口 */
	bad |= es8311_wr(0x0A, 0x10); /* ADC 串行口 */

	/* 电源路径 + ADC/DAC 启动（esp-bsp 现代序） */
	bad |= es8311_wr(0x0D, 0x01); /* 模拟上电 */
	bad |= es8311_wr(0x0E, 0x02); /* 模拟 PGA + ADC 调制器 */
	bad |= es8311_wr(0x12, 0x00); /* DAC 上电 */
	bad |= es8311_wr(0x13, 0x10); /* 耳机驱动输出使能 */
	bad |= es8311_wr(0x14, 0x1A); /* 模拟 MIC 路径（bit6=0 = AMIC）+ PGA */
	bad |= es8311_wr(0x15, 0x40); /* ADC ramp */
	bad |= es8311_wr(0x16, 0x24); /* MIC 增益 24dB（官方 init 定值）*/
	bad |= es8311_wr(0x17, 0x00); /* ADC 音量 0dB（0xBF 爬升起点的实测定值）*/
	bad |= es8311_wr(0x1B, 0x0A); /* ALC automute */
	bad |= es8311_wr(0x1C, 0x6A); /* ADC EQ 旁路 + 数字域直流消除 */
	bad |= es8311_wr(0x32, 0xB0); /* DAC 音量 ~70% */
	bad |= es8311_wr(0x37, 0x08); /* DAC EQ 旁路 */
	bad |= es8311_wr(0x44, 0x08);
	bad |= es8311_wr(0x45, 0x00);
	if (bad) {
		printk("[es8311] codec init i2c fail\n");
		return -3;
	}

	/* S3 I2S0 标准模式（TX 主机——BCK/WS 持续 = codec SCLK 取源） */
	gpio_pin_configure_dt(&pin_bck, GPIO_OUTPUT);
	gpio_pin_configure_dt(&pin_ws, GPIO_OUTPUT);
	gpio_pin_configure_dt(&pin_sdout, GPIO_OUTPUT);
	gpio_pin_configure_dt(&pin_sdin, GPIO_INPUT);
	rom_gpio_pad_select_gpio(pin_bck.pin);
	rom_gpio_matrix_out(pin_bck.pin, I2S0O_BCK_OUT_IDX, false, false);
	rom_gpio_pad_select_gpio(pin_ws.pin);
	rom_gpio_matrix_out(pin_ws.pin, I2S0O_WS_OUT_IDX, false, false);
	rom_gpio_pad_select_gpio(pin_sdout.pin);
	rom_gpio_matrix_out(pin_sdout.pin, I2S0O_SD_OUT_IDX, false, false);
	rom_gpio_pad_select_gpio(pin_sdin.pin);
	rom_gpio_pad_input_enable(pin_sdin.pin);
	rom_gpio_matrix_in(pin_sdin.pin, I2S0I_SD_IN_IDX, false);

	i2s_ll_enable_bus_clock(0, true);
	i2s_ll_reset_register(0);
	i2s_ll_enable_core_clock(i2s, true);

	/* mclk frac：XTAL 40M / (4 + 226/256) = 8.192MHz（精确） */
	hal_utils_clk_div_t mdiv = {.integer = 4, .numerator = 226,
				    .denominator = 256};

	/* 全双工共享总线配方（树内 i2s_esp32 驱动同序）：RX 先配、TX 后配
	 * （mclk_sel 终态 = tx），hal 序 enable_clock 先于 bind/src/mclk/bck；
	 * **sig_loopback（share BCK/WS）** = TX 时钟环回进 RX（第二坑）。 */
	i2s_ll_rx_reset(i2s);
	i2s_ll_rx_reset_fifo(i2s);
	i2s_ll_rx_enable_clock(i2s);
	i2s_ll_mclk_bind_to_rx_clk(i2s);
	i2s_ll_rx_clk_set_src(i2s, I2S_CLK_SRC_XTAL);
	i2s_ll_rx_set_mclk(i2s, &mdiv);
	i2s_ll_rx_set_bck_div_num(i2s, ES_BCK_DIV);
	i2s_ll_rx_set_slave_mod(i2s, false);
	i2s_ll_rx_set_sample_bit(i2s, 32, 32);
	i2s_ll_rx_set_half_sample_bit(i2s, 32);
	i2s_ll_rx_set_ws_width(i2s, 32);
	i2s_ll_rx_enable_msb_shift(i2s, true); /* Philips 1-bit 延迟 */
	i2s_ll_rx_select_std_slot(i2s, I2S_STD_SLOT_LEFT);
	i2s_ll_rx_enable_std(i2s);
	i2s_ll_rx_set_eof_num(i2s, ES_FRAME_DMA);

	i2s_ll_tx_reset(i2s);
	i2s_ll_tx_reset_fifo(i2s);
	i2s_ll_tx_enable_clock(i2s);
	i2s_ll_mclk_bind_to_tx_clk(i2s);
	i2s_ll_tx_clk_set_src(i2s, I2S_CLK_SRC_XTAL);
	i2s_ll_tx_set_mclk(i2s, &mdiv);
	i2s_ll_tx_set_bck_div_num(i2s, ES_BCK_DIV);
	i2s_ll_tx_set_slave_mod(i2s, false);
	i2s_ll_tx_set_sample_bit(i2s, 32, 32);
	i2s_ll_tx_set_half_sample_bit(i2s, 32);
	i2s_ll_tx_set_ws_width(i2s, 32);
	i2s_ll_tx_enable_msb_shift(i2s, true);
	i2s_ll_tx_select_std_slot(i2s, I2S_STD_SLOT_LEFT);
	i2s_ll_tx_enable_std(i2s);
	i2s_ll_share_bck_ws(i2s, true);

	/* GDMA：ch0 RX 环（EOF 轮询——pdm.c 同型）；ch1 TX 静音环常开 */
	gdma_ll_enable_bus_clock(0, true);
	gdma_ll_reset_register(0);
	gdma_ll_force_enable_reg_clock(gdma, true);
	gdma_ll_rx_reset_channel(gdma, GDMA_CH_RX);
	gdma_ll_rx_enable_owner_check(gdma, GDMA_CH_RX, false);
	gdma_ll_rx_connect_to_periph(gdma, GDMA_CH_RX, GDMA_PERIPH_I2S0);
	for (uint32_t i = 0; i < ES_RING_N; i++) {
		memset(&es_rx_desc[i].dw0, 0, sizeof(es_rx_desc[i].dw0));
		es_rx_desc[i].dw0.size = ES_FRAME_DMA;
		es_rx_desc[i].dw0.owner = 1;
		es_rx_desc[i].buffer = es_rx_buf[i];
		es_rx_desc[i].next = &es_rx_desc[(i + 1) % ES_RING_N];
	}
	es_rx_last_eof = 0;
	sys_cache_data_flush_range((void *)es_rx_desc, sizeof(es_rx_desc));
	gdma_ll_rx_set_desc_addr(gdma, GDMA_CH_RX,
				 (uint32_t)(uintptr_t)&es_rx_desc[0]);
	gdma_ll_rx_start(gdma, GDMA_CH_RX);

	gdma_ll_tx_reset_channel(gdma, GDMA_CH_TX);
	gdma_ll_tx_enable_owner_check(gdma, GDMA_CH_TX, false);
	gdma_ll_tx_connect_to_periph(gdma, GDMA_CH_TX, GDMA_PERIPH_I2S0);
	memset(es_tx_buf, 0, sizeof(es_tx_buf)); /* 静音环基线 */
	for (uint32_t i = 0; i < ES_RING_N; i++) {
		memset(&es_tx_desc[i].dw0, 0, sizeof(es_tx_desc[i].dw0));
		es_tx_desc[i].dw0.size = ES_FRAME_DMA;
		es_tx_desc[i].dw0.length = ES_FRAME_DMA; /* TX 必填传输字节数
			*（只设 size = 0 字节传输 → FIFO 永不填 → BCK 不出——
			* K2 实证第一坑；RX 无此字段语义〔pdm 先例〕）*/
		es_tx_desc[i].dw0.owner = 1;
		es_tx_desc[i].dw0.suc_eof = 1; /* 每槽 EOF（消费进度追踪）*/
		es_tx_desc[i].buffer = es_tx_buf[i];
		es_tx_desc[i].next = &es_tx_desc[(i + 1) % ES_RING_N];
	}
	es_tx_written = 0;
	sys_cache_data_flush_range((void *)es_tx_desc, sizeof(es_tx_desc));
	sys_cache_data_flush_range((void *)es_tx_buf, sizeof(es_tx_buf));
	gdma_ll_tx_set_desc_addr(gdma, GDMA_CH_TX,
				 (uint32_t)(uintptr_t)&es_tx_desc[0]);
	gdma_ll_tx_start(gdma, GDMA_CH_TX); /* 静音环常开（BCK/WS 不间断）*/

	/* 影子寄存器锁存（停止态配置生效前置——第五坑）+ 双域启动 */
	i2s_ll_tx_update(i2s);
	i2s_ll_rx_update(i2s);
	i2s_ll_rx_start(i2s);
	i2s_ll_tx_start(i2s);

	printk("[es8311] init: fs=%u bclk=%u slot=32bit ring=%ux%uB addr=0x%02x\n",
	       (unsigned)ES_FS_HZ, (unsigned)ES_BCLK_HZ, (unsigned)ES_RING_N,
	       (unsigned)ES_FRAME_DMA, ES8311_ADDR);
	es_inited = true;
	return 0;
}

/* ---- 录音（RX：32-bit 槽 → pcm16 mono 帧） ------------------------------- */

int ts_drv_es8311_capture(uint8_t *buf, uint32_t len)
{
	if (!es_inited || buf == NULL || len < ES_FRAME_PCM16) {
		return -1;
	}
	const uint32_t deadline = k_cycle_get_32() +
		k_ms_to_cyc_ceil64(500);
	uint32_t eof = 0;

	for (;;) {
		eof = gdma_ll_rx_get_success_eof_desc_addr(gdma, GDMA_CH_RX);
		if (eof != 0 && eof != es_rx_last_eof) {
			break;
		}
		if ((int32_t)(k_cycle_get_32() - deadline) >= 0) {
			printk("[es8311] no rx frame: eof=%08x last=%08x\n",
			       (unsigned)eof, (unsigned)es_rx_last_eof);
			es8311_silence_diag();
			return -2;
		}
		k_msleep(5);
	}
	es_rx_last_eof = eof;
	const dma_descriptor_align4_t *d =
		(const dma_descriptor_align4_t *)(uintptr_t)eof;

	sys_cache_data_invd_range(d->buffer, ES_FRAME_DMA);
	const int32_t *s32 = (const int32_t *)d->buffer;
	int16_t *s16 = (int16_t *)buf;

	for (uint32_t i = 0; i < ES_SAMPLES; i++) {
		s16[i] = (int16_t)(s32[i] >> 16); /* MSB 对齐 16-bit 样本 */
	}
	return (int)ES_FRAME_PCM16;
}

/* ---- 放音（TX：pcm16 mono 帧 → 32-bit 槽静音环打补丁） -------------------- */

int ts_drv_es8311_write(const uint8_t *buf, uint32_t len)
{
	if (!es_inited || buf == NULL || len != ES_FRAME_PCM16) {
		return -1;
	}
	/* 实时限流：已写 - 已消费 ≤ 1（环 4 槽余量 2——消费进度 = EOF 描述符）*/
	const uint32_t deadline = k_cycle_get_32() +
		k_ms_to_cyc_ceil64(500);

	for (;;) {
		uint32_t eof = gdma_ll_tx_get_eof_desc_addr(gdma, GDMA_CH_TX);
		uint32_t consumed = 0;

		if (eof != 0) {
			for (uint32_t i = 0; i < ES_RING_N; i++) {
				if (eof == (uint32_t)(uintptr_t)&es_tx_desc[i]) {
					consumed = i + 1;
					break;
				}
			}
		}
		uint32_t inflight = (es_tx_written + ES_RING_N - consumed) % ES_RING_N;

		if (inflight <= 1U) {
			uint32_t slot = (es_tx_written + 1U) % ES_RING_N;
			const int16_t *s16 = (const int16_t *)buf;
			int32_t *s32 = (int32_t *)es_tx_buf[slot];

			for (uint32_t i = 0; i < ES_SAMPLES; i++) {
				s32[i] = (int32_t)s16[i] << 16;
			}
			sys_cache_data_flush_range(es_tx_buf[slot], ES_FRAME_DMA);
			es_tx_written = (es_tx_written + 1U) % ES_RING_N;
			return (int)ES_FRAME_PCM16;
		}
		if ((int32_t)(k_cycle_get_32() - deadline) >= 0) {
			printk("[es8311] tx pacing timeout\n");
			return -2;
		}
		k_msleep(2);
	}
}

/* ---- bench 面 ------------------------------------------------------------- */

/* 功放使能（NS4168 SPK_EN 经 XL9555 port1 bit3——bench/启动面） */
int ts_drv_es8311_spk(bool on)
{
	return es8311_spk_en(on);
}

/* 静默/无帧诊断导出（bench 失败路径） */
void ts_drv_es8311_diag(void)
{
	es8311_silence_diag();
}
