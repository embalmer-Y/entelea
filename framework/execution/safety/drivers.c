/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 驱动分发（迁自旧 safety/driver_dispatch.c，R02 合同
 * framework/execution/safety/contract.md）——全库**唯一**出现 Zephyr
 * 输出驱动调用的文件（build-support/source-policy.json 保护门；
 * 架构机械检查的批准写路径）。写函数只依赖注册期冻结数据
 * （可重入/无锁）；失败进计数观测（void 写函数不报告成功与否的失败面）。
 * 相对旧实现调整：通用 GPIO 输入后端（hal/input 用）未迁入——estop 输入
 * 保留；prov 化 estop 边沿配置归 R03（R02 固定缺省上升沿）。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <entelea/safety.h>
#include "internal.h"

static struct entelea_ch_slot *slot_of(const entelea_out_channel_t *ch);

static void sim_record(struct entelea_ch_slot *s, const entelea_out_value_t *v)
{
	if (s == NULL) {
		return;
	}
	s->sim_rec.ring[s->sim_rec.head] = (entelea_write_rec_t){
		.t_ms = entelea_safety_now_ms(),
		.value_u = entelea_value_encode(s->desc->kind, *v),
	};
	s->sim_rec.head = (s->sim_rec.head + 1) % ENTELEA_SAFETY_SIM_REC_PER_CH;
	if (s->sim_rec.count < ENTELEA_SAFETY_SIM_REC_PER_CH) {
		s->sim_rec.count++;
	}
}

static int sim_write(const entelea_out_channel_t *ch, const entelea_out_value_t *v)
{
	/* 桩路径：真机后端同样先记录（golden 连续性）再写寄存器。
	 * 桩本身不失败——后端失败语义经真机路径返回值承担（ztest 用
	 * 假 PWM 设备注入）。 */
	sim_record(slot_of(ch), v);
	return 0;
}

static int sim_read(const entelea_out_channel_t *ch, entelea_out_value_t *out)
{
	struct entelea_ch_slot *s = slot_of(ch);

	if (s == NULL) {
		return -1;
	}
	*out = s->shadow;
	return 0;
}

/* ---- 真机 GPIO 输出后端（板级替换面） ----------------------------------
 * DT 绑定：zephyr,user 节点（uid = 通道 uid 串匹配；io-gpios = 输出脚）。
 * 写路径 = sim 记录 + 真寄存器写；uid 不匹配仍只记录。
 * init 由板级装配显式调用（entelea_drv_gpio_init）。缺 DT 属性 = -ENODEV
 * （配置错误如实上报）。 */
#if defined(ENTELEA_DRV_GPIO) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), uid)
static const struct gpio_dt_spec real_io =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), io_gpios);
static const char *const real_io_uid = DT_PROP(DT_PATH(zephyr_user), uid);
static bool real_io_ready;

int entelea_drv_gpio_init(void)
{
	if (!gpio_is_ready_dt(&real_io)) {
		return -EIO;
	}
	if (gpio_pin_configure_dt(&real_io, GPIO_OUTPUT_INACTIVE) != 0) {
		return -EIO;
	}
	real_io_ready = true;
	return 0;
}

static int gpio_real_write(const entelea_out_channel_t *ch,
			   const entelea_out_value_t *v)
{
	if (real_io_ready && strcmp(ch->uid, real_io_uid) == 0) {
		int rc = gpio_pin_set_dt(&real_io, v->b); /* 失败传播（缺陷 2） */

		if (rc != 0) {
			return rc; /* 失败不写 golden 记录（环形=已执行写） */
		}
	}
	sim_record(slot_of(ch), v);
	return 0;
}

#define ENTELEA_GPIO_WRITE gpio_real_write
#else /* 未启用/无 DT 绑定：桩路径（init 报错） */
int entelea_drv_gpio_init(void)
{
	return -ENODEV;
}

#define ENTELEA_GPIO_WRITE sim_write
#endif

/* ---- 真机 PWM 输出后端（板级替换面） ------------------------------------
 * DT 绑定：zephyr,user 节点（pwm-uid = 通道 uid；pwms 三元胞）。
 * 写路径 = sim 记录 + pwm_set（值域：u 低 16 = permille [0,1000]、
 * 高 16 = hz/100）。唯一写路径调用点：pwm_set 限定本文件。
 * 失败进 pwm_err 计数（观测面；无锁单写者递增）。 */
#if defined(ENTELEA_DRV_PWM) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwm_uid)
#include <zephyr/drivers/pwm.h>
static const struct pwm_dt_spec real_pwm = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));
static const char *const real_pwm_uid = DT_PROP(DT_PATH(zephyr_user), pwm_uid);
static bool real_pwm_ready;
static uint32_t pwm_err;

int entelea_drv_pwm_init(void)
{
	if (!device_is_ready(real_pwm.dev)) {
		return -ENODEV;
	}
	real_pwm_ready = true;
	return 0;
}

uint32_t entelea_drv_pwm_err_count(void)
{
	return pwm_err;
}

/* PWM 打包解码（合同）：u 低 16 = permille [0,1000]、高 16 = hz/100。
 * u==0（hz==0 且 permille==0）= 停止：占空 0（固定 1s 周期、脉宽 0），
 * 保证故障/安全态零值物理停止输出（缺陷 2）。hz==0 且 permille>0 或
 * permille>1000 = 非法编码。 */
static int pwm_decode(uint32_t u, uint32_t *period_ns, uint32_t *pulse_ns)
{
	uint32_t hz = (u >> 16) * 100U;
	uint32_t permille = u & 0xFFFFU;

	if (permille > 1000U) {
		return -EINVAL;
	}
	if (hz == 0U) {
		if (permille != 0U) {
			return -EINVAL;
		}
		*period_ns = 1000000000U;
		*pulse_ns = 0U;
		return 0;
	}
	uint32_t period = 1000000000U / hz;

	*period_ns = period;
	*pulse_ns = (uint32_t)((uint64_t)period * permille / 1000U);
	return 0;
}

static int pwm_real_write(const entelea_out_channel_t *ch,
			  const entelea_out_value_t *v)
{
	if (real_pwm_ready && strcmp(ch->uid, real_pwm_uid) == 0) {
		uint32_t period_ns;
		uint32_t pulse_ns;

		if (pwm_decode(v->u, &period_ns, &pulse_ns) != 0) {
			pwm_err++;
			return -EINVAL; /* 打包域外：显式失败，不静默不写 */
		}
		if (pwm_set(real_pwm.dev, real_pwm.channel, period_ns,
			    pulse_ns, real_pwm.flags) != 0) {
			pwm_err++; /* 后端失败传播：commit 不因此变 OK（缺陷 2） */
			return -EIO; /* 失败不写 golden 记录（环形=已执行写） */
		}
	}
	sim_record(slot_of(ch), v); /* 未绑定通道仅记录（板级只绑一路 PWM） */
	return 0;
}

#define ENTELEA_PWM_WRITE pwm_real_write
#else
int entelea_drv_pwm_init(void)
{
	return -ENODEV;
}

uint32_t entelea_drv_pwm_err_count(void)
{
	return 0;
}

#define ENTELEA_PWM_WRITE sim_write
#endif

const entelea_driver_ops_t entelea_drivers[ENTELEA_CH_KIND_COUNT] = {
	[ENTELEA_CH_GPIO] = {ENTELEA_GPIO_WRITE, sim_read},
	[ENTELEA_CH_PWM] = {ENTELEA_PWM_WRITE, sim_read},
	[ENTELEA_CH_POWER] = {sim_write, sim_read},
};

size_t entelea_driver_sim_writes(const char *uid, entelea_write_rec_t *out,
		size_t max)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return 0;
	}
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	size_t n = s->sim_rec.count < max ? s->sim_rec.count : max;

	for (size_t k = 0; k < n; k++) {
		size_t idx = (s->sim_rec.head + ENTELEA_SAFETY_SIM_REC_PER_CH -
			      s->sim_rec.count + k) %
			     ENTELEA_SAFETY_SIM_REC_PER_CH;

		out[k] = s->sim_rec.ring[idx];
	}
	return n;
}

/* estop DT 绑定与 ISR 粘合。绑定机制沿旧板级八实证：v4.4 起 chosen 非
 * zephyr 前缀属性不发射宏——走 aliases（DT_ALIAS(ts_estop_gpio) ←
 * overlay aliases { ts-estop-gpio = &node; }），语义与 DR-11 等价。 */
#define ESTOP_GPIO_NODE DT_ALIAS(ts_estop_gpio)

#if DT_NODE_EXISTS(ESTOP_GPIO_NODE)
static const struct gpio_dt_spec estop_spec = GPIO_DT_SPEC_GET(ESTOP_GPIO_NODE, gpios);
static struct gpio_callback estop_cb;

/* estop ISR：直达安全态，不经协议栈/调度排队（调用图纪律见 force.c）。 */
static void estop_isr(const struct device *dev, struct gpio_callback *cb,
		      uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	entelea_safety_force_all_fault();
}
#endif

entelea_result_t entelea_safety_estop_init(void)
{
#if DT_NODE_EXISTS(ESTOP_GPIO_NODE)
	if (!gpio_is_ready_dt(&estop_spec)) {
		return ENTELEA_E_IO;
	}
	gpio_init_callback(&estop_cb, estop_isr, BIT(estop_spec.pin));
	if (gpio_add_callback_dt(&estop_spec, &estop_cb) != 0) {
		return ENTELEA_E_IO;
	}
	/* 触发边沿：R02 固定缺省上升（prov 化配置面归 R03；
	 * 边沿映射纯函数另经 entelea_safety_estop_edge_of 供测试守卫）。 */
	if (gpio_pin_interrupt_configure_dt(&estop_spec,
			entelea_safety_estop_edge_of(0)) != 0) {
		return ENTELEA_E_IO;
	}
#endif
	return ENTELEA_OK; /* 无节点板（native_sim/测试）空操作；生产板必须提供 */
}

/* estop 边沿映射（0=上升缺省/1=下降/2=双沿；未知 fail-safe 上升）。 */
int entelea_safety_estop_edge_of(uint8_t flags)
{
	switch (flags) {
	case 1:
		return GPIO_INT_EDGE_FALLING;
	case 2:
		return GPIO_INT_EDGE_BOTH;
	case 0:
	default:
		return GPIO_INT_EDGE_RISING;
	}
}

static struct entelea_ch_slot *slot_of(const entelea_out_channel_t *ch)
{
	if (ch == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if (entelea_ch_table[i].desc == ch) {
			return &entelea_ch_table[i];
		}
	}
	return NULL;
}
