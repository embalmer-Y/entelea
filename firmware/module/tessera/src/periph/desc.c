/* SPDX-License-Identifier: Apache-2.0 */
/* 描述符表与注册职责链（LLD-ts-periph §2）。
 * 双名解耦：uid（逻辑——命名空间/权限/审计）与 dt_spec（物理——板相关）
 * 分离：换板只改 dt 绑定，uid 与 APP 权限不变（DEC-04 跨板迁移落点）。
 * 注册链：ts-periph（描述）→ ts-safety（GPIO/PWM 通道）或 ts-power（POWER
 * 槽→通道）→ ts-hal（实例寻址）。 */
#include <string.h>
#include <ts/hal.h>
#include <ts/periph.h>
#include <ts/power.h>
#include <zephyr/kernel.h>
#include "internal.h"

static ts_periph_desc_t descs[CONFIG_TS_PERIPH_MAX_DESCS]; /* DEC-27：24 */
static size_t desc_count;

static ts_dev_kind_t dev_kind_of(ts_periph_kind_t k)
{
	switch (k) {
	case TS_PK_GPIO:
		return TS_DEV_GPIO_OUT;
	case TS_PK_PWM:
		return TS_DEV_PWM;
	case TS_PK_POWER:
		return TS_DEV_POWER;
	case TS_PK_ADC:
		return TS_DEV_ADC;
	default:
		return TS_DEV_GPIO_OUT; /* 不可达（枚举封闭） */
	}
}

ts_res_t ts_periph_register(const ts_periph_desc_t *d)
{
	if (d == NULL || d->uid == NULL || d->uid[0] == '\0' ||
	    d->kind > TS_PK_ADC) {
		return TS_E_PARAM;
	}
	if (desc_count >= CONFIG_TS_PERIPH_MAX_DESCS) {
		return TS_E_NOMEM;
	}
	for (size_t i = 0; i < desc_count; i++) {
		if (strcmp(descs[i].uid, d->uid) == 0) {
			return TS_E_PARAM; /* 撞 uid（逻辑名全局唯一） */
		}
	}
	/* F-2（docs/history/review-archive）：先落静态表再注册——ts-safety 通道注册表持指针
	 * （slot->desc = ch），注册源必须是本表静态存储（与 ts-power slots 同型；
	 * 此前注册调用方 &d->safe，栈上组装即悬垂）。失败不递增 desc_count：
	 * 脏槽不可见（撞名检查只走 [0, desc_count)）；下游已注册项无回滚 =
	 * V1 已知限制（LLD-ts-periph §7——静态声明 + fail-fast 初始化下无
	 * 运行期影响）。 */
	ts_periph_desc_t *slot = &descs[desc_count];

	*slot = *d;
	if (slot->kind == TS_PK_ADC) {
		/* 输入侧：只进 ts-hal（不进 ts-safety——DR-13） */
	} else {
		/* 输出类：安全声明单源校验（safe.uid == uid，三态齐备由
		 * ts-safety/ts-power 注册校验兜底——fail 即整链失败） */
		if (slot->safe.uid == NULL || strcmp(slot->safe.uid, slot->uid) != 0) {
			return TS_E_PARAM;
		}
		ts_res_t rs;

		if (slot->kind == TS_PK_POWER) {
			/* POWER 经 ts-power 槽（linkloss/fault 由 power 生成；
			 * safe 三态中的 poweron.en = 上电是否供电）。槽记录由
			 * slots.c 静态自持——此处栈上组装安全。 */
			const ts_pwr_slot_t pwr = {
				.uid = slot->uid,
				.current_limit_ma = slot->safe.limits.current_limit_ma,
				.poweron_on = slot->safe.poweron.pwr.en,
			};
			rs = ts_power_register_slot(&pwr);
		} else {
			rs = ts_safety_register_channel(&slot->safe);
		}
		if (rs != TS_OK) {
			return rs;
		}
	}
	ts_hal_dev_desc_t dev = {
		.uid = slot->uid,
		.kind = dev_kind_of(slot->kind),
	};

	ts_res_t r = ts_hal_register_dev(&dev);

	if (r != TS_OK) {
		return r;
	}
	desc_count++;
	return TS_OK;
}

size_t ts_periph_count(void)
{
	return desc_count;
}

const ts_periph_desc_t *ts_periph_get(size_t idx)
{
	return idx < desc_count ? &descs[idx] : NULL;
}

#ifdef CONFIG_TS_TEST
void ts_periph_test_reset(void)
{
	memset(descs, 0, sizeof(descs));
	desc_count = 0;
}
#endif
