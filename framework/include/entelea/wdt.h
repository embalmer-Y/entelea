/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_WDT_H
#define ENTELEA_WDT_H

/* R02 软看门狗框架合同见 framework/runtime/supervision/contract.md。
 * 逾期 → ENTELEA_EVT_WDT_WARN → entelea_safety_system_fail。 */

#include <stdbool.h>
#include <stdint.h>
#include <entelea/result.h>
#include <entelea/supervision.h>
#include <entelea/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R02 源集合：仅 SYWORK（巡检自身活性）。旧 NET/APPMGR 源随其子系统
 * 批次加入，编号届时追加，不改既有值（DEC-48 注册表语义不变）。 */
typedef enum {
	ENTELEA_WDT_SYWORK,
	ENTELEA_WDT_COUNT,
} entelea_wdt_src_t;

/** 绑定实例时钟；未绑定调用 register/start 返回 ENTELEA_E_UNINIT，
 * 不伪造默认时基。绑定 NULL 恢复未绑定。
 * feed 可在 ISR 调用，此时绑定时钟的 read_ms 必须 ISR 安全
 * （平台 uptime 适配器满足；见 platform/zephyr 合同）。 */
entelea_result_t entelea_wdt_bind_clock(const entelea_clock_t *clock);

/** [thread] init 期注册喂狗源；period_ms 出处 DEC-22/27 语义。started 后拒绝。 */
entelea_result_t entelea_wdt_register(entelea_wdt_src_t src, uint32_t period_ms);

/** [any] 原子更新 last_feed；首次 feed = 激活（未激活源不参与逾期判定）。 */
void entelea_wdt_feed(entelea_wdt_src_t src);

/** [any] 声明式阻塞豁免窗（绝对截止 = now + budget；60s 钳制；feed 清除；
 * 不激活未激活源）。窗内不判逾；超窗恢复判逾（fail-safe 保持）。 */
void entelea_wdt_grace(entelea_wdt_src_t src, uint32_t budget_ms);

/** [thread] 去激活喂狗源（停止后无人喂是预期态，不判逾期；下次 feed 重新激活）。 */
void entelea_wdt_deactivate(entelea_wdt_src_t src);

/** [thread] 启动巡检（sysworkq 周期项，周期 = min(periods)/2；无注册源按
 * 硬上限巡检）。CONFIG_TASK_WDT 时挂 L3 通道（callback=NULL → 过期即
 * sys_reboot——软巡检卡死兜底）。失败显式返回，不报告成功。 */
entelea_result_t entelea_wdt_start(void);

/** [any] 巡检周期 = min(periods)/2（观测/测试用）。 */
uint32_t entelea_wdt_patrol_period_ms(void);

/** [any] 全局停机（system_fail 调用）：置位后巡检不喂、不重排、不判逾，
 * wdt_start 拒绝——任何线程触发的故障停机语义相同。一次性，无解除路径。 */
void entelea_wdt_halt(void);

/** [any] 时钟读取失败/回退累计（单调护栏沿用最后有效值期间的观测）。 */
uint32_t entelea_wdt_clock_faults(void);

/* 时钟失明策略（A1，owner 定案）：连续 ENTELEA_WDT_BLIND_ROUNDS 轮巡检
 * 时间不可知 → system_fail（载荷 0xFF 非源编号，标识时钟失明）。 */
#define ENTELEA_WDT_BLIND_ROUNDS 3
#define ENTELEA_FAIL_WDT_CLOCK_BLIND ENTELEA_FAIL_WDT(0xFFu)

/* 数值出处（R02 编译期常量；D12 板级冻结后随参考板调整）：
 * SYWORK 周期 1000ms、task_wdt 通道 5000ms、硬 WDT 上限 10000ms（DEC-48/22/27），
 * 豁免窗钳制 60000ms（DEC-64①）。 */
#define ENTELEA_WDT_SYWORK_PERIOD_MS 1000
#define ENTELEA_WDT_CHANNEL_MS 5000
#define ENTELEA_WDT_HARD_CAP_MS 10000
#define ENTELEA_WDT_GRACE_CAP_MS 60000

/* 仅 ENTELEA_TEST 构建：单次巡检判定（返回首个逾期源，无则 ENTELEA_WDT_COUNT）
 * 与豁免窗活性查询。 */
#ifdef ENTELEA_TEST
/** 测试隔离：源表/启动/halted/时钟护栏全清。 */
void entelea_wdt_test_reset(void);
entelea_wdt_src_t entelea_wdt_patrol_once(void);
bool entelea_wdt_grace_active(entelea_wdt_src_t src);
/** 确定性驱动一次巡检体（halted 语义测试用）；返回是否实际执行。 */
bool entelea_wdt_test_patrol_tick(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_WDT_H */
