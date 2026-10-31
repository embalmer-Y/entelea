/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 驱动分发（LLD-ts-safety §6）——全库**唯一**出现 Zephyr 输出驱动调用的文件
 * （L5 机械检查白名单 = 本文件；testing.md §3.1）。
 * M1：三 kind 均为 native_sim 桩（写序列环形记录 = L4 golden 数据源）；
 * 真机 GPIO/PWM/PMIC 驱动随板级移植替换，写函数只依赖注册期冻结数据（可重入/无锁）。
 */
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <ts/safety.h>
#include <ts/store.h>
#include "internal.h"

static struct ts_ch_slot *slot_of(const ts_out_ch_t *ch)
{
	for (size_t i = 0; i < ts_ch_count; i++) {
		if (ts_ch_table[i].desc == ch) {
			return &ts_ch_table[i];
		}
	}
	return NULL;
}

static void sim_record(struct ts_ch_slot *s, const ts_out_value_t *v)
{
	if (s == NULL) {
		return;
	}
	s->sim_rec.ring[s->sim_rec.head] = (ts_write_rec_t){
		.t_ms = ts_time_ms(),
		.value_u = ts_value_encode(s->desc->kind, *v),
	};
	s->sim_rec.head = (s->sim_rec.head + 1) % TS_SIM_REC_PER_CH;
	if (s->sim_rec.count < TS_SIM_REC_PER_CH) {
		s->sim_rec.count++;
	}
}

static void sim_write(const ts_out_ch_t *ch, const ts_out_value_t *v)
{
	/* native_sim 桩：真机此处为 gpio_pin_set / pwm_set_* 等唯一合法调用点。 */
	sim_record(slot_of(ch), v);
}

static int sim_read(const ts_out_ch_t *ch, ts_out_value_t *out)
{
	struct ts_ch_slot *s = slot_of(ch);

	if (s == NULL) {
		return -1;
	}
	*out = s->shadow;
	return 0;
}

/* ---- 真机 GPIO 输出后端（板级替换，DEC-44 IO 延迟实测起） ------------------
 * DT 绑定：zephyr,user 节点（uid = 通道 uid 串匹配；io-gpios = 输出脚）。
 * 写路径 = sim 记录（L4 golden 连续性）+ 真寄存器写；uid 不匹配仍只记录。
 * 只依赖注册期冻结数据（可重入/无锁——文件头纪律）。init 由板级 boot/bench
 * 显式调用（ts_drv_gpio_init）。缺 DT 属性 = -ENODEV（配置错误如实上报）。 */
#if defined(CONFIG_TS_DRV_GPIO)
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), uid)
/* Zephyr 4.4 无 ZEPHYR_USER_NODE（4.5 API）——用 DT_PATH(zephyr_user)。 */
static const struct gpio_dt_spec real_io =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), io_gpios);
static const char *const real_io_uid = DT_PROP(DT_PATH(zephyr_user), uid);
static bool real_io_ready;

int ts_drv_gpio_init(void)
{
	if (!gpio_is_ready_dt(&real_io)) {
		return -ENODEV;
	}
	if (gpio_pin_configure_dt(&real_io, GPIO_OUTPUT_INACTIVE) != 0) {
		return -EIO;
	}
	real_io_ready = true;
	return 0;
}

static void gpio_real_write(const ts_out_ch_t *ch, const ts_out_value_t *v)
{
	sim_record(slot_of(ch), v);
	if (real_io_ready && strcmp(ch->uid, real_io_uid) == 0) {
		gpio_pin_set_dt(&real_io, v->b);
	}
}

#define TS_GPIO_WRITE gpio_real_write
#else /* CONFIG_TS_DRV_GPIO 无 DT 绑定：回退桩（init 报错） */
int ts_drv_gpio_init(void)
{
	return -ENODEV;
}

#define TS_GPIO_WRITE sim_write
#endif
#else /* !CONFIG_TS_DRV_GPIO：native_sim/CI 基线路径 */
#define TS_GPIO_WRITE sim_write
#endif

/* ---- 真机 GPIO 输入后端（V0.4.0 M5/DR-02：输入中断驱动） --------------------
 * DT 绑定：zephyr,user 节点（gpio-in-uid = 输入通道 uid；in-gpios = 输入脚）。
 * 本文件纪律（L5 estop 调用图）= 中断上下文纯净：禁延迟/调度/打印/网络类
 * 内核 API（L5 正则全文匹配——注释措辞亦不可出现禁用词原形）；边沿回调
 * 只经**函数指针通知钩子**上抛（ts_drv_gpio_in_notify 注册，由 hal/input.c
 * 持有去抖工作项并触发唯一发布路径 ts_hal_input_poll_once——合同 9）。 */
#if defined(CONFIG_TS_DRV_GPIO)
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), gpio_in_uid)
static const struct gpio_dt_spec real_in =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), in_gpios);
static const char *const real_in_uid = DT_PROP(DT_PATH(zephyr_user), gpio_in_uid);
static bool real_in_ready;

const char *ts_drv_gpio_in_uid(void)
{
	return real_in_ready ? real_in_uid : NULL;
}

bool ts_drv_gpio_in_read(void)
{
	return real_in_ready && gpio_pin_get_dt(&real_in) == 1;
}

#if defined(CONFIG_TS_HAL_INPUT_EDGE)
static struct gpio_callback in_cb;
static void (*in_edge_notify)(void); /* ISR 上下文上抛钩子（无调度面） */

static void in_edge_cb(const struct device *port, struct gpio_callback *cb,
		       uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	if (in_edge_notify != NULL) {
		in_edge_notify(); /* 去抖/重轮询归 hal/input.c（L5 纪律） */
	}
}

int ts_drv_gpio_in_notify(void (*fn)(void))
{
	if (!real_in_ready) {
		return -ENODEV;
	}
	in_edge_notify = fn;
	return 0;
}
#endif /* TS_HAL_INPUT_EDGE */

int ts_drv_gpio_in_init(void)
{
	if (!gpio_is_ready_dt(&real_in)) {
		return -ENODEV;
	}
	if (gpio_pin_configure_dt(&real_in, GPIO_INPUT) != 0) {
		return -EIO;
	}
#if defined(CONFIG_TS_HAL_INPUT_EDGE)
	gpio_init_callback(&in_cb, in_edge_cb, BIT(real_in.pin));
	if (gpio_add_callback_dt(&real_in, &in_cb) != 0 ||
	    gpio_pin_interrupt_configure_dt(&real_in, GPIO_INT_EDGE_BOTH) != 0) {
		return -EIO;
	}
#endif
	real_in_ready = true;
	return 0;
}
#else /* 无输入 DT 绑定 */
int ts_drv_gpio_in_init(void) { return -ENODEV; }
bool ts_drv_gpio_in_read(void) { return false; }
const char *ts_drv_gpio_in_uid(void) { return NULL; }
#if defined(CONFIG_TS_HAL_INPUT_EDGE)
int ts_drv_gpio_in_notify(void (*fn)(void))
{
	ARG_UNUSED(fn);
	return -ENODEV;
}
#endif
#endif
#else /* !CONFIG_TS_DRV_GPIO */
int ts_drv_gpio_in_init(void) { return -ENODEV; }
bool ts_drv_gpio_in_read(void) { return false; }
const char *ts_drv_gpio_in_uid(void) { return NULL; }
#if defined(CONFIG_TS_HAL_INPUT_EDGE)
int ts_drv_gpio_in_notify(void (*fn)(void))
{
	ARG_UNUSED(fn);
	return -ENODEV;
}
#endif
#endif

/* ---- 真机 PWM 输出后端（板级九） -------------------------------------------
 * DT 绑定：zephyr,user 节点（pwm-uid = 通道 uid 串匹配；pwms = PWM 规格，
 * 三元胞 channel/period/flags——LEDC 的引脚路由经 ledc0 pinctrl，不在本 spec）。
 * 写路径 = sim 记录（L4 golden 连续性）+ pwm_set（值域：u 打包低 16 = permille
 * [0,1000]、高 16 = hz/100——api 层已保证 hz≥100 即打包值非零，安全态值同守）。
 * 唯一写路径调用点（L5）：pwm_set 限定本文件。写函数 void 返回——失败进
 * pwm_err 计数（观测面；无锁单写者递增）。init 由板级 boot/bench 显式调用。 */
#if defined(CONFIG_TS_DRV_PWM)
#include <zephyr/drivers/pwm.h>
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwm_uid)
static const struct pwm_dt_spec real_pwm = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));
static const char *const real_pwm_uid = DT_PROP(DT_PATH(zephyr_user), pwm_uid);
static bool real_pwm_ready;
static uint32_t pwm_err;

int ts_drv_pwm_init(void)
{
	if (!device_is_ready(real_pwm.dev)) {
		return -ENODEV;
	}
	real_pwm_ready = true;
	return 0;
}

uint32_t ts_drv_pwm_err_count(void)
{
	return pwm_err;
}

static void pwm_real_write(const ts_out_ch_t *ch, const ts_out_value_t *v)
{
	sim_record(slot_of(ch), v);
	if (!real_pwm_ready || strcmp(ch->uid, real_pwm_uid) != 0) {
		return;
	}
	uint32_t hz = (v->u >> 16) * 100U;
	uint32_t permille = v->u & 0xFFFFU;

	if (hz == 0U || permille > 1000U) {
		pwm_err++; /* 打包域外（api/安全态守卫失效的兜底观测） */
		return;
	}
	uint32_t period_ns = 1000000000U / hz;
	uint64_t pulse_ns = (uint64_t)period_ns * permille / 1000U;

	if (pwm_set(real_pwm.dev, real_pwm.channel, period_ns,
		    (uint32_t)pulse_ns, real_pwm.flags) != 0) {
		pwm_err++;
	}
}

#define TS_PWM_WRITE pwm_real_write
#else /* CONFIG_TS_DRV_PWM 无 DT 绑定：回退桩（init 报错） */
int ts_drv_pwm_init(void)
{
	return -ENODEV;
}

uint32_t ts_drv_pwm_err_count(void)
{
	return 0;
}

#define TS_PWM_WRITE sim_write
#endif
#else /* !CONFIG_TS_DRV_PWM：native_sim/CI 基线路径 */
#define TS_PWM_WRITE sim_write
#endif

const ts_driver_ops_t ts_drivers[TS_CH_KIND_COUNT] = {
	[TS_CH_GPIO] = {TS_GPIO_WRITE, sim_read},
	[TS_CH_PWM] = {TS_PWM_WRITE, sim_read},
	[TS_CH_POWER] = {sim_write, sim_read},
};

size_t ts_driversim_writes(const char *uid, ts_write_rec_t *out, size_t max)
{
	int i = ts_ch_find(uid);

	if (i < 0) {
		return 0;
	}
	struct ts_ch_slot *s = &ts_ch_table[i];
	size_t n = s->sim_rec.count < max ? s->sim_rec.count : max;

	for (size_t k = 0; k < n; k++) {
		size_t idx = (s->sim_rec.head + TS_SIM_REC_PER_CH - s->sim_rec.count + k) %
			     TS_SIM_REC_PER_CH;
		out[k] = s->sim_rec.ring[idx];
	}
	return n;
}

/* estop DT 绑定（DR-11）与 ISR 粘合。
 * 绑定机制（板级八实证修订）：v4.4 的 EDT 管道不发射非 zephyr 前缀 chosen
 * 属性的宏（dtlib 层属性在、edtlib 层被弃）——改走 **aliases**
 * （DT_ALIAS(ts_estop_gpio) ← overlay aliases { ts-estop-gpio = &node; }），
 * 语义与 DR-11 等价（板级声明式 estop 引脚绑定）。 */

#define ESTOP_GPIO_NODE DT_ALIAS(ts_estop_gpio) /* 板 overlay aliases 定义 */

#if DT_NODE_EXISTS(ESTOP_GPIO_NODE)
static const struct gpio_dt_spec estop_spec = GPIO_DT_SPEC_GET(ESTOP_GPIO_NODE, gpios);
static struct gpio_callback estop_cb;

/* estop ISR：直达安全态，不经协议栈/调度排队（合同 5；L5 调用图审计目标）。 */
static void estop_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	ts_safety_force_all_fault();
}
#endif

ts_res_t ts_safety_estop_init(void)
{
#if DT_NODE_EXISTS(ESTOP_GPIO_NODE)
	if (!gpio_is_ready_dt(&estop_spec)) {
		return TS_E_IO;
	}
	/* 触发沿随 prov 配置（DR-11 prov 化收口，单元 F）：
	 * estop_trigger_flags 0=上升（缺省，兼容既有行为）/1=下降/2=双沿/
	 * 其他 = fail-safe 上升（静默——本文件调用图纪律禁打印原语）。
	 * boot 步骤 1 先于网络步骤的 prov 装载——此处先显式 load（幂等只读；
	 * 失败 = 缺省上升）。 */
	(void)ts_store_prov_load();
	const uint8_t flags = ts_store_prov()->estop_trigger_flags;

	gpio_init_callback(&estop_cb, estop_isr, BIT(estop_spec.pin));
	if (gpio_add_callback_dt(&estop_spec, &estop_cb) != 0) {
		return TS_E_IO;
	}
	if (gpio_pin_interrupt_configure_dt(&estop_spec,
					    ts_safety_estop_edge_of(flags)) != 0) {
		return TS_E_IO;
	}
#endif
	return TS_OK; /* 无节点板（native_sim/测试）空操作；生产板必须提供（M2+ 板级强化） */
}

/* estop 边沿映射（导出供 framework.safety 用例断言；单元 F）。
 * 未知值 = fail-safe 上升（静默——本文件调用图纪律禁打印原语；
 * 非法 prov 值的观测面 = framework.safety 用例 + 部署期 prov 校验）。 */
int ts_safety_estop_edge_of(uint8_t flags)
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
