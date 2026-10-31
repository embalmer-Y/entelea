/* SPDX-License-Identifier: Apache-2.0 */
/* 实例注册 + ts_api_v1 实现（LLD-ts-hal §3/§4）。
 * 输出路径：写类 API 全部收敛到 ts_safety_commit（零直接驱动调用——L5）。 */
#include <string.h>
#include <errno.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h>
#ifdef CONFIG_TS_STORE_KV
#include <ts/store.h> /* V0.4.0 M9：kv 后端（DR-15） */
#endif
#include <zephyr/kernel.h>

/* ---- 实例注册（registry） ------------------------------------------------ */

#define MAX_DEVS CONFIG_TS_HAL_MAX_INSTANCES /* DEC-27 #7：24 */

/* hz 打包进 u16（值/100）：上限 65535*100 Hz（打包域定义，impl-review IR-08）；
 * 下界 100 Hz = 打包粒度（真机 PWM 后端按 hz 求周期，打包值 0 = 周期无穷——
 * 板级九真驱动接线起在 api 层显式拒绝，V1 桩曾静默接受 hz<100） */
#define PWM_HZ_MIN 100U
#define PWM_HZ_MAX 6553500U

static ts_hal_dev_desc_t devices[MAX_DEVS];
static size_t dev_count;

ts_res_t ts_hal_register_dev(const ts_hal_dev_desc_t *desc)
{
	if (desc == NULL || desc->uid == NULL || desc->uid[0] == '\0') {
		return TS_E_PARAM;
	}
	if (dev_count >= MAX_DEVS) return TS_E_NOMEM;
	devices[dev_count++] = *desc;
	return TS_OK;
}

size_t ts_hal_dev_count(void) { return dev_count; }
const ts_hal_dev_desc_t *ts_hal_dev_get(size_t idx)
{
	return idx < dev_count ? &devices[idx] : NULL;
}

/* 实例号 → uid 查找（写路径用） */
static const ts_hal_dev_desc_t *find_dev(uint8_t inst, ts_dev_kind_t kind)
{
	if (inst >= dev_count) return NULL;
	const ts_hal_dev_desc_t *d = &devices[inst];

	return d->kind == kind ? d : NULL;
}

/* ---- ts_api_v1（LLD-ts-hal §3）------------------------------------------- */

ts_res_t ts_gpio_write(ts_ctx_t c, uint8_t inst, bool v)
{
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_GPIO, TS_PERM_OP_WRITE, inst);

	if (r != TS_OK) return r;
	const ts_hal_dev_desc_t *d = find_dev(inst, TS_DEV_GPIO_OUT);

	if (d == NULL) return TS_E_NOTFOUND;
	/* 唯一写路径：经 ts-safety 保护层（合同 2）；actor = 调用方 app_id
	 * （IR2-08/DEC-48⑥：审计归因） */
	ts_out_value_t val = {.b = v};

	return ts_safety_commit_a(d->uid, val, c.app_id);
}

ts_res_t ts_gpio_read(ts_ctx_t c, uint8_t inst, bool *out)
{
	if (out == NULL) return TS_E_PARAM;
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_GPIO, TS_PERM_OP_READ, inst);

	if (r != TS_OK) return r;
	/* 输入直读（合同 3：输入不受保护层影响）——V0.4.0 M5：真后端 uid
	 * 匹配直读（driver_dispatch 输入面）；未匹配 = false 近似（sim 连续） */
	const ts_hal_dev_desc_t *d = find_dev(inst, TS_DEV_GPIO_IN);

	if (d == NULL) return TS_E_NOTFOUND;
	*out = ts_gpio_in_read_fw(d);
	return TS_OK;
}

bool ts_gpio_in_read_fw(const ts_hal_dev_desc_t *d)
{
	if (d == NULL) return false;
	const char *uid = ts_drv_gpio_in_uid();

	return (uid != NULL && strcmp(d->uid, uid) == 0)
		       ? ts_drv_gpio_in_read()
		       : false;
}

/* ---- APP 业务状态 kv（V0.4.0 M9，DR-15）---------------------------------- */

ts_res_t ts_kv_save(ts_ctx_t c, uint32_t key_hash, const uint8_t *data,
		    uint16_t len)
{
#ifdef CONFIG_TS_STORE_KV
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_KV, TS_PERM_OP_WRITE, 0);

	if (r != TS_OK) return r; /* 越权留痕由 ts_perm_check 承担（合同 10） */
	return ts_store_kv_save(key_hash, data, len);
#else
	ARG_UNUSED(c);
	ARG_UNUSED(key_hash);
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return TS_E_STATE; /* 未编入 kv 面（CONFIG_TS_STORE_KV=n）——如实 */
#endif
}

int32_t ts_kv_load(ts_ctx_t c, uint32_t key_hash, uint8_t *out, uint16_t max)
{
#ifdef CONFIG_TS_STORE_KV
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_KV, TS_PERM_OP_READ, 0);

	if (r != TS_OK) return r;
	if (out == NULL || max == 0) return TS_E_PARAM;
	return ts_store_kv_load(key_hash, out, max);
#else
	ARG_UNUSED(c);
	ARG_UNUSED(key_hash);
	ARG_UNUSED(out);
	ARG_UNUSED(max);
	return TS_E_STATE; /* 未编入 kv 面（CONFIG_TS_STORE_KV=n）——如实 */
#endif
}

ts_res_t ts_pwm_set(ts_ctx_t c, uint8_t inst, uint32_t hz, uint16_t permille)
{
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_PWM, TS_PERM_OP_SET, inst);

	if (r != TS_OK) return r;
	if (hz < PWM_HZ_MIN || hz > PWM_HZ_MAX || permille > 1000) {
		return TS_E_PARAM; /* 打包域界限（IR-08 + 板级九下界），防静默截断 */
	}
	const ts_hal_dev_desc_t *d = find_dev(inst, TS_DEV_PWM);

	if (d == NULL) return TS_E_NOTFOUND;
	/* Hz 与 permille 打包进 u32（低 16 = permille，高 16 = Hz/100） */
	uint32_t u = ((uint32_t)(hz / 100) << 16) | (permille & 0xFFFFU);
	ts_out_value_t val = {.u = u};

	return ts_safety_commit_a(d->uid, val, c.app_id);
}

/* ---- 真机 ADC 输入后端（板级九；LLD-ts-hal §3 输入侧） ---------------------
 * 合同 3：输入直读不经保护层——驱动调用在 hal 输入面（L5 唯一写路径检查
 * 只辖输出驱动，adc_read 非输出面）。
 * DT 绑定：zephyr,user 节点（adc-uid = 逻辑名匹配；io-channels = <&adcN ch>）。
 * 12bit / 内部基准 / 12dB 衰减（ADC_GAIN_1_4 = 最宽量程）；mV 换算走 Zephyr
 * 通用口径（esp32 驱动已将 raw 预补偿：eFuse 校准 + 衰减反归一）。 */
#if defined(CONFIG_TS_DRV_ADC)
#include <zephyr/drivers/adc.h>
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), adc_uid)
static const struct adc_dt_spec real_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static const char *const real_adc_uid = DT_PROP(DT_PATH(zephyr_user), adc_uid);
static bool real_adc_ready;

/* 出处：esp32s3 SoC 12bit 分辨率上限（SOC_ADC_DIGI_MAX_BITWIDTH）；
 * 口径 = LLD-ts-hal §3 ADC */
#define TS_ADC_RESOLUTION 12
/* Source: 上游 adc_esp32.c ADC_ESP32_DEFAULT_VREF_INTERNAL = 1100（推导：
 * esp32 驱动把 raw 预补偿到该口径——eFuse 校准 + 衰减反归一） */
#define TS_ADC_REF_MV 1100

int ts_adc_drv_init(void)
{
	if (!device_is_ready(real_adc.dev)) {
		return -ENODEV;
	}
	const struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_1_4,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = real_adc.channel_id,
		.differential = 0,
	};
	if (adc_channel_setup(real_adc.dev, &cfg) != 0) {
		return -EIO;
	}
	real_adc_ready = true;
	return 0;
}

static int adc_sample_mv(int32_t *mv)
{
	uint16_t raw = 0;
	const struct adc_sequence seq = {
		.channels = BIT(real_adc.channel_id),
		.buffer = &raw,
		.buffer_size = sizeof(raw),
		.resolution = TS_ADC_RESOLUTION,
	};

	if (adc_read(real_adc.dev, &seq) != 0) {
		return -EIO;
	}
	int32_t v = raw;

	adc_raw_to_millivolts(TS_ADC_REF_MV, ADC_GAIN_1_4,
			      TS_ADC_RESOLUTION, &v);
	*mv = v;
	return 0;
}

/* 非可调常量（编译期结构标志：真 ADC 后端已编译且 DT 绑定存在——推导自
 * CONFIG_TS_DRV_ADC + adc_uid 属性存在性，无默认值语义） */
#define TS_ADC_REAL_AVAILABLE 1
#else /* CONFIG_TS_DRV_ADC 无 DT 绑定：init 报错，读路径恒走桩 */
int ts_adc_drv_init(void)
{
	return -ENODEV;
}
#endif
#else /* !CONFIG_TS_DRV_ADC：native_sim/CI 基线（桩） */
#endif

ts_res_t ts_adc_read(ts_ctx_t c, uint8_t inst, int32_t *mv)
{
	if (mv == NULL) return TS_E_PARAM;
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_ADC, TS_PERM_OP_READ, inst);

	if (r != TS_OK) return r;
	const ts_hal_dev_desc_t *d = find_dev(inst, TS_DEV_ADC);

	if (d == NULL) return TS_E_NOTFOUND;
#ifdef TS_ADC_REAL_AVAILABLE
	/* 真后端（uid 匹配 + 已 init）：真读；读失败如实 TS_E_IO。
	 * uid 不匹配/未 init = 桩通道（sim/L4 连续性）。 */
	if (real_adc_ready && strcmp(d->uid, real_adc_uid) == 0) {
		return adc_sample_mv(mv) == 0 ? TS_OK : TS_E_IO;
	}
#endif
		*mv = 0;
		return TS_OK;
}

ts_res_t ts_adc_sample_fw(uint8_t inst, int32_t *mv)
{
	/* 框架侧采样（G3，单元 F）：input monitor 观测路径——无 ctx/权限面
	 * （合同 3 观测侧；APP 侧仍走 ts_adc_read 权限裁决）。真后端判定与
	 * ts_adc_read 同源（uid 匹配 + init）；未绑定 = 桩 0mV（sim 连续性）。 */
	const ts_hal_dev_desc_t *d = find_dev(inst, TS_DEV_ADC);

	if (d == NULL) return TS_E_NOTFOUND;
#ifdef TS_ADC_REAL_AVAILABLE
	if (real_adc_ready && strcmp(d->uid, real_adc_uid) == 0) {
		return adc_sample_mv(mv) == 0 ? TS_OK : TS_E_IO;
	}
#endif
	*mv = 0;
	return TS_OK;
}

uint64_t ts_time_ms_api(ts_ctx_t c)
{
	ARG_UNUSED(c);
	return ts_time_ms(); /* 合同 9：唯一时间源（LLD-ts-hal §3） */
}

ts_res_t ts_log_write(ts_ctx_t c, uint8_t lvl, const char *msg, uint32_t len)
{
	/* V0.4.0 M2（IR-16 收口）：日志面真实化——经 ts-hal 裁决（log:write:0，
	 * 单实例类）+ 全局窗口限速（V1 单全局预算；多 APP 分账随 M7）。
	 * 输出前缀 = c.app_id（绑定层身份，与 native 侧 rt_id 同源）。 */
	if (msg == NULL || len == 0) return TS_E_PARAM;
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_LOG, TS_PERM_OP_WRITE, 0);

	if (r != TS_OK) return r; /* 越权留痕由 ts_perm_check 承担（合同 10） */
	static uint32_t win_start, win_used;
	static bool dropping;
	uint32_t now = (uint32_t)ts_time_ms();

	if (now - win_start >= 1000u) {
		win_start = now;
		win_used = 0;
		dropping = false;
	}
	if (win_used >= CONFIG_TS_LOG_RATE_PER_S) {
		if (!dropping) {
			dropping = true;
			printk("[hal] log rate limit: dropping (cap %u/s)\n",
			       CONFIG_TS_LOG_RATE_PER_S);
		}
		return TS_E_BUSY; /* 背压可见（军规 7） */
	}
	win_used++;
	printk("[app%u/l%u] %s\n", (unsigned)c.app_id, (unsigned)lvl, msg);
	return TS_OK;
}
