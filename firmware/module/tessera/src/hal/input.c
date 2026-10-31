/* SPDX-License-Identifier: Apache-2.0 */
/* 输入周期采集与变化上报（LLD-ts-hal §5，DR-02；G3 路由 = 单元 F）。
 * sysworkq 周期工作项（无独立线程）；只观测不改值（合同 3 观测侧落点）。
 * V1 采集面 = ADC 实例（真后端经 ts_adc_sample_fw；未绑定 = 桩 0mV 连续）；
 * 变化语义 = **传输级**（与上次采样不同即发）——阈值/滞回语义归 APP 逻辑
 * （D4/D6 演示即此分工；框架不引入未裁默认值）。数字 GPIO_IN 直读留桩
 * （无板级绑定面；DR-11 后续）。
 * 消费：ts_evt 总线（TS_EVT_INPUT_CHANGED，ts-net pub 外发 + ts-appmgr
 * 订阅路由进 APP mailbox——runtime.c，G3）。 */
#include <string.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/safety.h> /* ts_drv_gpio_in_*（M5 输入后端钩子/读面） */
#include <zephyr/kernel.h>

static struct k_work_delayable input_work;
static bool started;
static int32_t last_mv[CONFIG_TS_HAL_MAX_INSTANCES];
static bool have_last[CONFIG_TS_HAL_MAX_INSTANCES];

#if defined(CONFIG_TS_HAL_INPUT_EDGE)
static struct k_work_delayable in_debounce;

static void in_edge_work(struct k_work *w)
{
	ARG_UNUSED(w);
	ts_hal_input_poll_once(); /* 唯一发布路径（合同 9） */
}

static void in_edge_kick(void)
{
	/* ISR 上下文（驱动回调上抛）——仅提交去抖工作项 */
	k_work_reschedule_for_queue(&k_sys_work_q, &in_debounce,
				     K_MSEC(CONFIG_TS_HAL_INPUT_DEBOUNCE_MS));
}
#endif

#ifdef CONFIG_TS_TEST
/* 测试注入（G3 用例：无真后端时驱动变化序列） */
static bool inj_used[CONFIG_TS_HAL_MAX_INSTANCES];
static int32_t inj_mv[CONFIG_TS_HAL_MAX_INSTANCES];

void ts_hal_input_test_inject(uint32_t inst, int32_t mv)
{
	if (inst < CONFIG_TS_HAL_MAX_INSTANCES) {
		inj_used[inst] = true;
		inj_mv[inst] = mv;
	}
}
#endif

static void input_poll_fn(struct k_work *work)
{
	struct k_work_delayable *d = k_work_delayable_from_work(work);

	ts_hal_input_poll_once();
	k_work_reschedule_for_queue(&k_sys_work_q, d,
				     K_MSEC(CONFIG_TS_HAL_INPUT_POLL_MS));
}

void ts_hal_input_poll_once(void)
{
	for (size_t i = 0; i < ts_hal_dev_count(); i++) {
		const ts_hal_dev_desc_t *d = ts_hal_dev_get(i);
		int32_t mv = 0;
		bool valid = false;

		if (d == NULL) {
			continue;
		}
		if (d->kind == TS_DEV_ADC) {
			int32_t v = 0;
#ifdef CONFIG_TS_TEST
			if (i < CONFIG_TS_HAL_MAX_INSTANCES && inj_used[i]) {
				v = inj_mv[i]; /* 注入优先（测试确定性） */
				valid = true;
			} else
#endif
			if (ts_adc_sample_fw((uint8_t)i, &v) == TS_OK) {
				valid = true; /* 读失败如实跳过本拍（下拍重试） */
			}
			mv = v;
		} else if (d->kind == TS_DEV_GPIO_IN) {
			/* V0.4.0 M5/DR-02：数字输入纳入监测——传输级 0/1
			 * （真后端直读见 api.c ts_gpio_in_read_fw；边沿回调
			 * 模式只加速本函数，发布路径唯一〔合同 9〕）。 */
			bool bv = false;
#ifdef CONFIG_TS_TEST
			if (i < CONFIG_TS_HAL_MAX_INSTANCES && inj_used[i]) {
				bv = inj_mv[i] != 0;
				valid = true;
			} else
#endif
			{
				bv = ts_gpio_in_read_fw(d);
				valid = true;
			}
			mv = bv ? 1 : 0;
		} else {
			continue;
		}
		if (!valid) {
			continue;
		}
		if (have_last[i] && last_mv[i] != mv) {
			const struct ts_input_evt payload = {
				.inst = (uint32_t)i,
				.old_mv = last_mv[i],
				.new_mv = mv,
			};
			const ts_evt_t evt = {
				.id = TS_EVT_INPUT_CHANGED,
				.t_ms = ts_time_ms(),
				.data = &payload,
				.len = sizeof(payload),
			};
			ts_evt_publish(&evt);
		}
		last_mv[i] = mv;
		have_last[i] = true;
	}
}

ts_res_t ts_hal_input_start(void)
{
	if (started) return TS_E_STATE;
	started = true;
	k_work_init_delayable(&input_work, input_poll_fn);
	k_work_reschedule_for_queue(&k_sys_work_q, &input_work,
				     K_MSEC(CONFIG_TS_HAL_INPUT_POLL_MS));
#if defined(CONFIG_TS_HAL_INPUT_EDGE)
	/* M5/DR-02：边沿加速——去抖工作项本文件持有，钩子注册（未绑定真输入
	 * 〔sim/无 DT〕= 如实失败，轮询语义不变）。 */
	(void)ts_drv_gpio_in_notify(in_edge_kick);
#endif
	return TS_OK;
}
