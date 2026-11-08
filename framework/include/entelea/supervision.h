/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_SUPERVISION_H
#define ENTELEA_SUPERVISION_H

/* R02 监督启动合同见 framework/runtime/supervision/contract.md。
 * 监督与业务 boot 拆分：本模块只提供顺序执行器与 fail-safe 语义；
 * 步骤表由平台装配（platform/zephyr/boot_steps.c），顺序是规格，
 * 只允许尾部追加，不可调换既有次序。 */

#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef entelea_result_t (*entelea_boot_step_fn)(void);

typedef struct {
	const char *name;
	entelea_boot_step_fn fn;
} entelea_boot_step_t;

/* fail-safe reason 编码（沿旧 TS_FAIL_* 位布局，来源域 <<16 | 载荷）。 */
#define ENTELEA_FAIL_SRC_BOOT 0x0001U
#define ENTELEA_FAIL_SRC_WDT 0x0002U
#define ENTELEA_FAIL_BOOT(idx) \
	((uint32_t)ENTELEA_FAIL_SRC_BOOT << 16 | (uint32_t)(idx))
#define ENTELEA_FAIL_WDT(src) \
	((uint32_t)ENTELEA_FAIL_SRC_WDT << 16 | (uint32_t)(src))

/** [thread] 固定顺序执行；每步完成后发布 ENTELEA_EVT_BOOT_STEP（含失败步
 * result）。任一步失败 → entelea_safety_system_fail(ENTELEA_FAIL_BOOT(i))
 * 后受控停机（停喂 WDT → 硬 WDT 兜底复位；ENTELEA_TEST 构建停机路径可返回，
 * 由用例断言）。全部成功 → 发布 ENTELEA_EVT_BOOT_DONE 后返回；
 * 调用方（main）随后转常驻监督空闲。 */
void entelea_supervision_boot(const entelea_boot_step_t *steps, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_SUPERVISION_H */
