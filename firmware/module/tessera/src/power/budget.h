/* SPDX-License-Identifier: Apache-2.0 */
/* ts-power 内部共享（不对外）。 */
#ifndef TS_POWER_INTERNAL_H__
#define TS_POWER_INTERNAL_H__

#include <stdint.h>

/* budget.c 供给 slots.c：预算总额与已用量（读回权威） */
uint32_t ts_power_budget_ma(void);
uint32_t ts_power_used_ma(void);

#ifdef CONFIG_TS_TEST
void ts_power_budget_test_reset(void); /* 预算注入/峰值清零（slots.test_reset 汇总调用） */
#endif

#endif /* TS_POWER_INTERNAL_H__ */
