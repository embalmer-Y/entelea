/* SPDX-License-Identifier: Apache-2.0 */
/*
 * estop / fail-safe 直达路径（迁自旧 safety/force.c，R02 合同
 * framework/execution/safety/contract.md）。
 * estop 路径纪律：force_all_fault 调用图内禁分配、队列、锁、协议栈符号；
 * 仅原子置位 + 驱动直写（drivers.c 同纪律）。
 * 相对旧实现调整：noinit 留痕持久化归 R03（R02 仅内存 reason 可观）；
 * estop 沿触发入 flash 审计环归 R03。
 */
#include <entelea/safety.h>
#include "internal.h"

static uint32_t fail_reason_store;

/* estop ISR 直达：[ISR] 安全——仅原子置位与直写（不经队列/调度）。 */
void entelea_safety_force_all_fault(void)
{
	atomic_set(&entelea_forced, 1);
	atomic_set(&entelea_forced_at,
		   (atomic_val_t)(uint32_t)entelea_safety_now_ms());

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		entelea_drivers[s->desc->kind].write(s->desc, &s->desc->fault);
		s->shadow = s->desc->fault;
		s->state = ENTELEA_ST_SAFE_FAULT;
		s->have_last = false;
	}
	/* 事后补发 ENTELEA_EVT_ESTOP：由 sysworkq 周期项检测 forced 上升沿后
	 * 发布（ISR 内不提交工作项——队列禁令）；响应上界 = 巡检周期 + 调度延迟。 */
}

void entelea_safety_system_fail(uint32_t reason)
{
	fail_reason_store = reason;
	atomic_set(&entelea_forced, 1);
	atomic_set(&entelea_forced_at,
		   (atomic_val_t)(uint32_t)entelea_safety_now_ms());

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		entelea_drivers[s->desc->kind].write(s->desc, &s->desc->fault);
		s->shadow = s->desc->fault;
		s->state = ENTELEA_ST_SAFE_FAULT;
		s->have_last = false;
	}
	/* noinit 留痕（复位后归因）归 R03 存储批次。 */

#ifdef ENTELEA_TEST
	return; /* 测试构建：置态后返回，由用例断言停机路径 */
#endif
	/* 受控停机：停喂硬 WDT → 兜底复位（复位原因留痕归 R03）。 */
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

#ifdef ENTELEA_TEST
uint32_t entelea_safety_test_fail_reason(void)
{
	return fail_reason_store;
}
#endif

/* estop 事后补发（sysworkq 周期驱动；由 wdt 巡检复用同一节奏） */
static bool estop_announced;

void entelea_safety_estop_announce_reset(void)
{
	estop_announced = false; /* clear_fault 释放锁存时复位（二次 estop 可再补发） */
}

void entelea_safety_estop_deferred_publish(void)
{
	if (atomic_get(&entelea_forced) != 0 && !estop_announced) {
		estop_announced = true;
		const entelea_estop_evt_t payload = {
			.t_ms = (uint32_t)atomic_get(&entelea_forced_at),
		};
		const entelea_event_t evt = {
			.id = ENTELEA_EVT_ESTOP,
			.data = &payload,
			.len = sizeof(payload),
		};
		entelea_evt_publish(&evt);
	}
}
