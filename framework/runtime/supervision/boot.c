/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 监督启动执行器（迁自旧 core/boot.c 的编排语义，R02 合同
 * framework/runtime/supervision/contract.md）。
 * 拆分调整：步骤表由平台装配（旧版表与实现同文件编译期拼接，业务步骤
 * net/app/audit 混入 core）；新版执行器只消费平台传入的表，顺序规格
 * （estop → 全通道安全起始态 → WDT → 绑定就绪，尾部追加）由
 * platform/zephyr/boot_steps.c 持有并由 ztest 守卫。
 * 任一步失败 → 全系统 fail-safe，不得半启动（旧合同 6 语义）。
 */
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <zephyr/kernel.h>

void entelea_supervision_boot(const entelea_boot_step_t *steps, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		entelea_result_t res = steps[i].fn();
		/* 每步完成后发布 BOOT_STEP（含失败步的 result）——重放观测点。 */
		const entelea_boot_step_evt_t payload = {
			.idx = (uint8_t)i,
			.result = res,
			.name = steps[i].name,
		};
		const entelea_event_t evt = {
			.id = ENTELEA_EVT_BOOT_STEP,
			.data = &payload,
			.len = sizeof(payload),
		};

		entelea_evt_publish(&evt);
		if (res != ENTELEA_OK) {
			/* 任一步失败 → 全系统 fail-safe，不得半启动。
			 * 受控停机：停喂硬 WDT → 兜底复位（noinit 留痕归 R03）。 */
			entelea_safety_system_fail(ENTELEA_FAIL_BOOT((uint32_t)i));
			for (;;) {
				k_sleep(K_FOREVER);
			}
		}
	}

	const entelea_event_t done = {
		.id = ENTELEA_EVT_BOOT_DONE,
	};

	entelea_evt_publish(&done);
	/* 返回后调用方（main）转常驻监督空闲。 */
}
