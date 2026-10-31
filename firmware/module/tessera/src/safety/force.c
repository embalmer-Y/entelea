/* SPDX-License-Identifier: Apache-2.0 */
/*
 * estop / fail-safe 直达路径（LLD-ts-safety §5）——合同 5/8。
 * estop 路径纪律（L5 机械检查目标）：ts_safety_force_all_fault 调用图内禁
 * 分配、队列、锁、协议栈符号；仅原子置位 + driver_dispatch 直写。
 */
#include <ts/safety.h>
#ifdef CONFIG_TS_STORE_AUDIT
#include <ts/store.h>
#endif
#include "internal.h"

/* noinit 留痕经 ts-store（DR-01/17，M2 交付）；弱桩保持接缝。 */
__weak void ts_store_noinit_record(uint32_t reason)
{
	/* M2：写入 noinit 区供复位后诊断 */
	ARG_UNUSED(reason);
}

/* estop ISR 直达：[ISR] 安全——仅原子置位与直写（不经队列/调度，合同 5）。 */
void ts_safety_force_all_fault(void)
{
	atomic_set(&ts_forced, 1);
	atomic_set(&ts_forced_at, (atomic_val_t)(uint32_t)ts_time_ms());

	for (size_t i = 0; i < ts_ch_count; i++) {
		struct ts_ch_slot *s = &ts_ch_table[i];

		ts_drivers[s->desc->kind].write(s->desc, &s->desc->fault);
		s->shadow = s->desc->fault;
		s->state = TS_ST_SAFE_FAULT;
		s->have_last = false;
	}
	/* 事后补发 TS_EVT_ESTOP：由 sysworkq 周期项检测 forced 上升沿后发布
	 * （ISR 内不提交工作项——队列禁令）；响应上界 = 巡检周期 + 调度延迟。 */
}

static uint32_t fail_reason_store;

void ts_safety_system_fail(uint32_t reason)
{
	fail_reason_store = reason;
	atomic_set(&ts_forced, 1);
	atomic_set(&ts_forced_at, (atomic_val_t)(uint32_t)ts_time_ms());

	for (size_t i = 0; i < ts_ch_count; i++) {
		struct ts_ch_slot *s = &ts_ch_table[i];

		ts_drivers[s->desc->kind].write(s->desc, &s->desc->fault);
		s->shadow = s->desc->fault;
		s->state = TS_ST_SAFE_FAULT;
		s->have_last = false;
	}
	ts_store_noinit_record(reason);

	if (IS_ENABLED(CONFIG_TS_TEST)) {
		return; /* 测试构建：置态后返回，由用例断言停机路径 */
	}
	/* 受控停机：停喂硬 WDT → 兜底复位（复位原因已留痕 noinit，合同 4/6）。 */
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

#ifdef CONFIG_TS_TEST
uint32_t ts_safety_test_fail_reason(void)
{
	return fail_reason_store;
}
#endif

/* estop 事后补发（sysworkq 周期驱动；由 wdt 巡检复用同一节奏） */
static bool estop_announced;

void ts_safety_estop_announce_reset(void)
{
	estop_announced = false; /* clear_fault 释放锁存时复位（IR-01：二次 estop 可再补发） */
}

void ts_safety_estop_deferred_publish(void)
{
	if (atomic_get(&ts_forced) != 0 && !estop_announced) {
		estop_announced = true;
		const ts_estop_evt_t payload = {
			.t_ms = (uint32_t)atomic_get(&ts_forced_at),
		};
		const ts_evt_t evt = {
			.id = TS_EVT_ESTOP,
			.t_ms = ts_time_ms(),
			.data = &payload,
			.len = sizeof(payload),
		};
		ts_evt_publish(&evt);
#ifdef CONFIG_TS_STORE_AUDIT
		/* DEC-52①：estop 沿触发入 flash 审计环（本函数 = sysworkq 线程
		 * 上下文——ISR 直达路径禁 flash 写，故挂补发点；evt=1 表 estop） */
		const ts_audit_entry_t au = {
			.t_ms = ts_time_ms(),
			.ch_idx = 0xFFFF,
			.res = TS_E_STATE,
			.actor = 0,
			.kind = 0,
			.value_u = payload.t_ms,
		};

		(void)ts_store_audit_append(&au, 1);
#endif
	}
}
