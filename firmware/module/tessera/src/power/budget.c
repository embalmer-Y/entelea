/* SPDX-License-Identifier: Apache-2.0 */
/* 功率预算记账与上报（LLD-ts-power §3）。
 * 预算总额来自 prov（板级 provisioning，运行时只读——合同 10）：
 * prov 未加载/未配置 → 0（= 一切供电请求拒绝，缺省即安全侧）。
 * 记账 = 各槽 ts_safety_readback 的 Σ(ma)（读回权威，不另设影子账本——
 * 单源纪律）；超预算不在线降额（既有槽不受影响，行为可预测）。 */
#include <string.h>
#include <ts/power.h>
#include <ts/safety.h>
#include <ts/store.h>

static uint32_t peak_ma;

#ifdef CONFIG_TS_TEST
static uint32_t test_budget_ma; /* 测试注入（prod 不编译——prov 是唯一预算源） */
#endif

uint32_t ts_power_budget_ma(void)
{
#ifdef CONFIG_TS_TEST
	if (test_budget_ma != 0) {
		return test_budget_ma; /* 测试注入优先（显式覆盖 prov） */
	}
#endif
	const ts_prov_t *prov = ts_store_prov();

	return prov->power_budget_ma;
}

uint32_t ts_power_used_ma(void)
{
	uint32_t sum = 0;

	for (size_t i = 0; i < ts_power_slot_count(); i++) {
		const ts_pwr_slot_t *s = ts_power_slot_get(i);
		ts_out_value_t rb;

		if (s != NULL && ts_safety_readback(s->uid, &rb) == TS_OK && rb.pwr.en) {
			sum += rb.pwr.ma;
		}
	}
	if (sum > peak_ma) {
		peak_ma = sum;
	}
	return sum;
}

void ts_power_budget_snapshot(ts_power_budget_t *out)
{
	if (out == NULL) {
		return;
	}
	out->budget_ma = ts_power_budget_ma();
	out->used_ma = ts_power_used_ma(); /* 峰值在遍历中顺带更新 */
	out->peak_ma = peak_ma;
	out->slots = (uint8_t)ts_power_slot_count();
}

#ifdef CONFIG_TS_TEST
void ts_power_test_set_budget(uint32_t budget_ma)
{
	test_budget_ma = budget_ma;
}

void ts_power_budget_test_reset(void)
{
	test_budget_ma = 0;
	peak_ma = 0;
}
#endif
