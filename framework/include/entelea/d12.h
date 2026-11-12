/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_D12_H
#define ENTELEA_D12_H

/* D12 产品阈值运行时闸（owner 2026-11-10 批准 v1 保守方案——
 * docs/d12-threshold-proposal.md）。集中三面：
 *   冷却：E_BUSY 后 60s 冷却窗；异常复位后 60s 业务冷却窗
 *   频度：滚动 60s 窗 ≤6 次 + 自启动累计 ≤200 次；没有跨复位日历
 *         计数，任何重启都可能清零，不等价于获批的每天 200 次上限
 *   风暴：连续 3 次失败闩（不自动恢复——外部 storm_clear 命令面；
 *         重绑时钟不释放）
 * 计数单位=一次传输请求（model_ask 重试逐次过闸/上报——RF1 G5）。
 * 判定纯同步（无定时器）：调用方经 *_allow 前置问、*_note 后置报。
 * 时间经单调夹紧（回退沿用最后有效值，窗/冷却只延不缩）；上报冷却时
 * 时钟失明则挂起，首次好读起算（不丢）；无时钟/读失败 = 拒（E_STATE）。 */

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
 * 由平台启动防护调用；跨复位计数留存归平台（判定见 reset_guard）。 */
void entelea_d12_boot_note(bool abnormal_reset);

/** 异常复位风暴阈值：连续 ≥3 次异常复位 → 当次启动 SAFE-HALT
 * （POR 起算即"第 4 次启动"——D12 v1 §9）。 */
#define ENTELEA_D12_RESET_STORM_MAX 3U

/** 跨复位风暴判定（纯策略，I/O 归平台）。abnormal=本次复位异常类；
 * history_ok=留存计数可读且有效。*count 入=留存值、出=应写回值。
 * 返回 true=SAFE-HALT。异常且历史不可知 → fail-closed；纯 POR 清零；
 * 计数饱和不回绕；count 为 NULL 时亦须 SAFE-HALT。平台复位原因不可知、
 * 留存设备不可用或读写失败时，无论复位类型均须 SAFE-HALT。 */
bool entelea_d12_reset_guard(bool abnormal, bool history_ok,
			     uint32_t *count);

/** 模型调用前置判定（冷却/频度/风暴/依赖无关——依赖面归 model_ask）。
 * OK=允许；E_BUSY=冷却/频度；E_STATE=风暴闩。 */
entelea_result_t entelea_d12_model_allow(void);

/** 模型调用结果上报（成功清风暴计数；E_BUSY 触发冷却；失败累加风暴）。 */
void entelea_d12_model_note(entelea_result_t r);

/** 风暴闩状态与外部释放（命令面/到场）；未绑时钟报闭锁。 */
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
