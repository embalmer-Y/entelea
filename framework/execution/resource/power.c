/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 功率预算记账与受控供电提交（迁自旧 power/budget.c + slots 检查-提交
 * 原子化语义，R04 合同见本目录 contract.md）。
 * 预算源 = prov（运行时只读，合同 10）；测试注入仅 ENTELEA_TEST。
 * 记账单源 = safety readback（不设影子账本——与物理声明一致优先）。
 * 检查-提交原子化：与 safety commit 同一 write_lock（锁序复用，无新锁）。
 */
#include <string.h>
#include "internal.h"
#include <entelea/prov.h>
#include <entelea/resource.h>
#include <entelea/safety.h>

static uint32_t peak_ma;

#ifdef ENTELEA_TEST
static uint32_t test_budget_ma; /* prod 不编译——prov 是唯一预算源 */
#endif

uint32_t entelea_power_budget_ma(void)
{
#ifdef ENTELEA_TEST
	if (test_budget_ma != 0) {
		return test_budget_ma; /* 测试注入（显式覆盖 prov） */
	}
#endif
	return entelea_prov()->power_budget_ma;
}

uint32_t entelea_power_used_ma(void)
{
	uint32_t sum = 0;

	for (size_t i = 0; i < entelea_ch_count; i++) {
		const struct entelea_ch_slot *s = &entelea_ch_table[i];
		entelea_out_value_t rb;

		if (s->desc != NULL && s->desc->kind == ENTELEA_CH_POWER &&
		    entelea_safety_readback(s->desc->uid, &rb) == ENTELEA_OK &&
		    rb.pwr.en) {
			sum += rb.pwr.ma;
		}
	}
	if (sum > peak_ma) {
		peak_ma = sum;
	}
	return sum;
}

void entelea_power_snapshot(entelea_power_snapshot_t *out)
{
	if (out == NULL) {
		return;
	}
	out->budget_ma = entelea_power_budget_ma();
	out->used_ma = entelea_power_used_ma();
	out->peak_ma = peak_ma;
	out->slots = 0;
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if (entelea_ch_table[i].desc != NULL &&
		    entelea_ch_table[i].desc->kind == ENTELEA_CH_POWER) {
			out->slots++;
		}
	}
}

/* SET 门：R04 受控供电要求 power:write 声明（实例 0——单预算面）。 */
static entelea_result_t power_gate(entelea_principal_t ctx)
{
	return entelea_perm_check(ctx, ENTELEA_PERM_POWER, ENTELEA_PERM_WRITE, 0);
}

entelea_result_t entelea_power_request(entelea_principal_t ctx,
				       const char *safety_uid,
				       entelea_out_value_t v)
{
	if (safety_uid == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = power_gate(ctx);

	if (r != ENTELEA_OK) {
		return r;
	}
	/* 原子检查-提交（review P1 修复）：预算检查与 commit 同一
	 * write_lock 临界区——并发 power_request 不再能双过检查超预算。 */
	entelea_safety_write_lock();
	if (v.pwr.en && v.pwr.ma > 0) {
		uint32_t budget = entelea_power_budget_ma();
		uint32_t used = entelea_power_used_ma();
		uint32_t current = 0;
		entelea_out_value_t rb;

		if (entelea_safety_readback(safety_uid, &rb) == ENTELEA_OK &&
		    rb.pwr.en) {
			current = rb.pwr.ma; /* 本通道替换语义：先扣自身 */
		}
		if (v.pwr.ma > budget || used - current > budget - v.pwr.ma) {
			entelea_safety_write_unlock();
			return ENTELEA_E_RANGE; /* 超预算整笔拒绝（无在线降额） */
		}
	}
	r = entelea_safety_commit_locked(safety_uid, v, NULL);
	entelea_safety_write_unlock();
	return r;
}

#ifdef ENTELEA_TEST
void entelea_power_test_set_budget(uint32_t budget_ma)
{
	test_budget_ma = budget_ma;
}

void entelea_power_test_reset(void)
{
	test_budget_ma = 0;
	peak_ma = 0;
}
#endif
