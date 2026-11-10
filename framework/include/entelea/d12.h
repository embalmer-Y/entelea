/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_D12_H
#define ENTELEA_D12_H

/* D12 产品阈值运行时闸（owner 2026-11-10 批准 v1 保守方案——
 * docs/d12-threshold-proposal.md）。集中三面：
 *   冷却：E_BUSY 后 60s 冷却窗；异常复位后 60s 业务冷却窗
 *   频度：滚动 60s 窗 ≤6 次 + 自启动累计 ≤200 次（无 RTC——"日"
 *         语义如实降级为启动累计，重启清零=重启本身需到场/异常留痕）
 *   风暴：连续 3 次失败闩（不自动恢复——外部 storm_clear 命令面）
 * 判定纯同步（无定时器）：调用方经 *_allow 前置问、*_note 后置报。 */

#include <stdbool.h>
#include <stdint.h>
#include <entelea/time.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 绑定时钟（判定依赖）。 */
entelea_result_t entelea_d12_bind_clock(const entelea_clock_t *clock);

/** 平台启动上报：本次复位是否异常类（WDT/panic——决定冷却窗）。
 * 在 boot 步骤前调用；同时复位风暴计数由平台层管（noinit）。 */
void entelea_d12_boot_note(bool abnormal_reset);

/** 模型调用前置判定（冷却/频度/风暴/依赖无关——依赖面归 model_ask）。
 * OK=允许；E_BUSY=冷却/频度；E_STATE=风暴闩。 */
entelea_result_t entelea_d12_model_allow(void);

/** 模型调用结果上报（成功清风暴计数；E_BUSY 触发冷却；失败累加风暴）。 */
void entelea_d12_model_note(entelea_result_t r);

/** 风暴闩状态与外部释放（命令面/到场）。 */
bool entelea_d12_storm_latched(void);
void entelea_d12_storm_clear(void);

/** wasm 执行前置判定（冷却窗内拒——E_BUSY；风暴不影响 wasm）。 */
entelea_result_t entelea_d12_wasm_allow(void);

/** 观测面：自启动调用次数 / 被闸拒绝次数。 */
uint32_t entelea_d12_model_calls(void);
uint32_t entelea_d12_model_blocked(void);

#ifdef ENTELEA_TEST
void entelea_d12_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_D12_H */
