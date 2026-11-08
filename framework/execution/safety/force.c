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
#include <entelea/wdt.h>
#include "internal.h"

static uint32_t fail_reason_store;
atomic_t entelea_system_failed = ATOMIC_INIT(0);

#ifdef ENTELEA_TEST
void entelea_safety_test_reset_system_failed(void)
{
	atomic_set(&entelea_system_failed, 0);
}
#endif

/* estop ISR 直达：[ISR] 安全——仅原子置位与直写（不经队列/调度）。
 * 写失败在 ISR 上下文不可返回：计入失败计数（可观测），状态/影子仍按
 * 合同迁移（物理未知由 fail-safe/WDT 收口）。 */
void entelea_safety_force_all_fault(void)
{
	bool any_write_failed = false;

	atomic_set(&entelea_forced, 1);
	atomic_set(&entelea_forced_at,
		   (atomic_val_t)(uint32_t)entelea_safety_now_ms());

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (entelea_drivers[s->desc->kind].write(s->desc, &s->desc->fault) != 0) {
			/* 写失败：物理效果未知——不伪称 fault 值已落（shadow
			 * 留最后确认值）；状态仍撤销资格并标记仅 recover
			 * 可恢复（三轮 review 发现 2）。 */
			entelea_safety_count_write_failure();
			s->state = ENTELEA_ST_SAFE_FAULT;
			s->fault_by_write_failure = true;
			/* 基线保留：失败=无确认写（决定 C）。 */
			any_write_failed = true;
			continue;
		}
		s->shadow = s->desc->fault; /* 确认写：覆盖早期“物理未知”标记 */
		s->fault_by_write_failure = false;
		s->last_t = entelea_safety_now_ms(); /* 确认写刷新锚点 */
		s->have_last = true;
		s->state = ENTELEA_ST_SAFE_FAULT;
	}
	if (any_write_failed) {
		/* 强停升级（owner 2026-11-08 定案①）：estop 强停落驱失败 =
		 * 无法保证输出达安全值 → 全局停机（halt=原子置位，ISR 安全；
		 * 停喂后由 WDT 复位收口）。先尽力安全化其余通道再升级。 */
		entelea_wdt_halt();
	}
	/* 事后补发 ENTELEA_EVT_ESTOP：由 sysworkq 周期项检测 forced 上升沿后
	 * 发布（ISR 内不提交工作项——队列禁令）；响应上界 = 巡检周期 + 调度延迟。 */
}

void entelea_safety_system_fail(uint32_t reason)
{
	fail_reason_store = reason;
	/* 全局停机先行（缺陷 4）：任何线程触发 system_fail，软 WDT 巡检
	 * 立即停喂停排——停机语义不依赖调用线程自身挂起。 */
	entelea_wdt_halt();
	atomic_set(&entelea_system_failed, 1); /* 一次性：clear_fault 拒绝释放 */
	atomic_set(&entelea_forced, 1);
	atomic_set(&entelea_forced_at,
		   (atomic_val_t)(uint32_t)entelea_safety_now_ms());

	for (size_t i = 0; i < entelea_ch_count; i++) {
		struct entelea_ch_slot *s = &entelea_ch_table[i];

		if (entelea_drivers[s->desc->kind].write(s->desc, &s->desc->fault) != 0) {
			entelea_safety_count_write_failure();
			s->state = ENTELEA_ST_SAFE_FAULT;
			s->fault_by_write_failure = true;
			/* 基线保留：失败=无确认写（决定 C）。 */
			continue;
		}
		s->shadow = s->desc->fault; /* 确认写：覆盖早期“物理未知”标记 */
		s->fault_by_write_failure = false;
		s->last_t = entelea_safety_now_ms(); /* 确认写刷新锚点 */
		s->have_last = true;
		s->state = ENTELEA_ST_SAFE_FAULT;
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
