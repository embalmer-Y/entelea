/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_DELIVERY_H
#define ENTELEA_DELIVERY_H

/* R12 工厂/试用/晋升合同见 framework/services/delivery/contract.md。
 * 编程循环=OpenHands Software Agent SDK（owner 已确认决策）——SDK 侧
 * 循环不在固件内；Forge 侧集成契约（窄接口）与固件试用状态机在此。
 * 稳定/候选分离、单候选、试用数据隔离、用户内容绑定确认。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 试用状态机（单候选槽——D05 已批方向） ---- */

typedef enum {
	ENTELEA_TRIAL_NONE = 0,
	ENTELEA_TRIAL_PENDING_CONFIRM, /* 包已就位：待用户 hash/权限确认 */
	ENTELEA_TRIAL_ACTIVE,          /* 用户已确认：试用运行中 */
	ENTELEA_TRIAL_REJECTED,        /* 用户拒绝：槽保留可重推 */
	ENTELEA_TRIAL_TIMEOUT,         /* 确认窗超时：fail-closed 撤销 */
} entelea_trial_state_t;

typedef struct {
	uint8_t digest[32];   /* 候选包 sha256（用户确认对象） */
	uint16_t permissions; /* 权限位图（用户确认对象） */
	uint8_t version;
} entelea_trial_candidate_t;

/** [thread] 提交候选（进入 PENDING_CONFIRM；已有 ACTIVE=BUSY 拒——
 * 单候选）。digest/permissions 即用户确认对象（内容绑定）。 */
entelea_result_t entelea_trial_submit(const entelea_trial_candidate_t *cand);

/** [thread] 用户确认（PENDING→ACTIVE；确认前状态/权限不生效）。 */
entelea_result_t entelea_trial_confirm(void);

/** [thread] 用户拒绝（PENDING→REJECTED；槽保留可重推）。 */
entelea_result_t entelea_trial_reject(void);

/** [thread] 超时撤销（PENDING→TIMEOUT；fail-closed）。 */
entelea_result_t entelea_trial_timeout(void);

/** [thread] 结束试用（ACTIVE→NONE；候选槽清空——晋升或放弃后）。 */
entelea_result_t entelea_trial_end(void);

/** [any] 状态/当前候选（观测）。 */
entelea_trial_state_t entelea_trial_state(void);
void entelea_trial_current(entelea_trial_candidate_t *out);

/* ---- Forge 集成契约（窄接口——SDK 侧循环消费；固件消费产物） ---- */

typedef struct {
	uint8_t package_digest[32]; /* TSAP 包 sha256（验签后内容身份） */
	uint16_t permissions;       /* 申请权限位图 */
	uint8_t runtime_version;    /* Wasm/Lua 载荷版本 */
	uint8_t payload_kind;       /* 0=wasm 1=lua */
	uint32_t budget_steps;      /* 执行预算 */
	uint32_t mem_quota_kb;     /* 内存配额 */
	char description[48];       /* 候选说明（用户读） */
} entelea_factory_candidate_t;

/** 固件侧对工厂产物的验收面（factory_candidate → trial_candidate）：
 * 权限/预算域外拒；payload_kind 未知拒（不默认解释）。 */
entelea_result_t entelea_factory_intake(const entelea_factory_candidate_t *fc);

/* ---- 跨重启持久（板级批 A-4；经 R03 meta 双副本） ---- */

/** [thread] 状态落盘（含 NONE 态——清槽也持久）。调用时机归装配层
 * （试用事件稀疏：meta 乒乓磨损可接受；产品磨损策略随 D05 细节）。 */
entelea_result_t entelea_trial_flush(void);

/** [boot] 恢复（幂等）：无记录/坏记录（CRC/版本/域外）→ fail-closed
 * 置 NONE（不复活垃圾态——TIMEOUT/REJECTED 等约束态跨重启保持）。 */
entelea_result_t entelea_trial_restore(void);

#ifdef ENTELEA_TEST
void entelea_trial_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_DELIVERY_H */
