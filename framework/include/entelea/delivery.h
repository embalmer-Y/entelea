/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_DELIVERY_H
#define ENTELEA_DELIVERY_H

/* R12 工厂/试用/晋升合同见 framework/services/delivery/contract.md。
 * 编程循环=OpenHands Software Agent SDK（owner 已确认决策）——SDK 侧
 * 循环不在固件内；Forge 侧集成契约（窄接口）与固件试用状态机在此。
 * 单候选与内容绑定试用确认已有；稳定集准入和试用数据晋升尚未闭合。 */

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
	ENTELEA_TRIAL_UNINITIALIZED,   /* 尚未读取持久状态 */
} entelea_trial_state_t;

#define ENTELEA_TRIAL_SLOT_UNKNOWN 0xFFU /* 坏记录恢复：候选槽不可知 */
#define ENTELEA_TRIAL_PERM_MAX 96U       /* 权限列表字节上限（分号分隔，不含 NUL） */
#define ENTELEA_TRIAL_STEPS_MAX 100000000U /* 确认步数上限 = 设备 Wasm 配额上限 */

/* 用户确认的对象：展示 = 确认 = 执行绑定。digest 只由设备计算。 */
typedef struct {
	uint8_t digest[32];                /* 验签后整文件 sha256（设备计算） */
	char perm[ENTELEA_TRIAL_PERM_MAX]; /* 权限列表，如 "gpio:read:0"；空 = 无权限 */
	uint8_t perm_len;
	uint32_t budget_steps;             /* 确认步数（执行配额 = min(请求, 此值, 设备上限)） */
	uint8_t version;
	uint8_t slot;                      /* 候选所在 app slot */
	uint32_t epoch;                    /* 提交纪元：submit 赋值（输入忽略），确认须回显 */
} entelea_trial_candidate_t;

/* 提交请求：宿主只提出权限、步数与版本，不能指定摘要。 */
typedef struct {
	uint8_t slot;
	uint8_t version;
	uint32_t budget_steps;
	char perm[ENTELEA_TRIAL_PERM_MAX];
	uint8_t perm_len;
} entelea_trial_request_t;

/* 执行门授予的权限与步数（仅 ACTIVE 且槽与摘要相符时有效）。 */
typedef struct {
	char perm[ENTELEA_TRIAL_PERM_MAX];
	uint8_t perm_len;
	uint32_t budget_steps;
} entelea_trial_grant_t;

/** [thread] 提交候选（进入 PENDING_CONFIRM）。设备先验签槽内包，再以该包的
 * 整文件摘要作为候选身份；宿主无法指定摘要。权限列表须符合
 * entelea_perm_parse_list 文法（否则 E_PARAM），步数须在 1..ENTELEA_TRIAL_
 * STEPS_MAX（否则 E_RANGE）。ACTIVE/PENDING 期=BUSY（单候选，已展示对象
 * 不可被替换）；REJECTED/TIMEOUT 期换槽=BUSY（槽保留，重推须同槽）。槽内
 * 无有效包时返回验签错误且不改变状态。纪元耗尽=E_RANGE 且不改变候选。
 * 首次操作先恢复持久状态。 */
entelea_result_t entelea_trial_submit(const entelea_trial_request_t *req);

/** [thread] 用户确认（PENDING→ACTIVE）。shown=向用户展示并获其确认的
 * 对象（取自 trial_current）：digest/权限列表/步数/version/slot/epoch
 * 任一不符=E_PERM 且保持 PENDING（旧展示/重推后的过期确认不生效）。
 * 调用方负责真实用户授权，本 API 不验证用户身份或交互。 */
entelea_result_t entelea_trial_confirm(const entelea_trial_candidate_t *shown);

/** [thread] 用户拒绝（PENDING→REJECTED；槽保留可重推）。 */
entelea_result_t entelea_trial_reject(void);

/** [thread] 超时撤销（PENDING→TIMEOUT；fail-closed）。 */
entelea_result_t entelea_trial_timeout(void);

/** [thread] 结束试用（ACTIVE→NONE；候选槽清空——晋升或放弃后）。 */
entelea_result_t entelea_trial_end(void);

/** [any] 状态/当前候选（观测）。首次恢复前状态为 UNINITIALIZED，候选槽
 * 为 UNKNOWN；查询本身不读取存储。调用方须串行化状态机及存储操作。 */
entelea_trial_state_t entelea_trial_state(void);
void entelea_trial_current(entelea_trial_candidate_t *out);

/** [thread] 执行门（装载/调用前由 wasm 宿主查询）：digest=该 slot 当前
 * 整文件 sha256。NONE 或非候选 slot=OK（试用政策不管辖；稳定集准入
 * 归 D05，且执行时不授予 natives 权限——见 grant）；候选 slot（或候选
 * slot 不可知）仅 ACTIVE 且 digest 相符=OK，否则 E_PERM（PENDING/
 * REJECTED/TIMEOUT/确认后换字节均拒）。 */
entelea_result_t entelea_trial_gate(uint8_t slot, const uint8_t digest[32]);

/** [thread] 授权查询（执行时逐次调用，不缓存）：仅当 ACTIVE 且槽与摘要
 * 均与确认对象相符时返回 OK 并填入确认的权限列表与步数；否则 E_PERM，
 * 调用方不得授予任何 natives 权限。试用结束后下一次查询即失效。 */
entelea_result_t entelea_trial_grant(uint8_t slot, const uint8_t digest[32],
				     entelea_trial_grant_t *out);

/* ---- Forge 集成契约（窄接口——SDK 侧循环消费；固件消费产物） ---- */

typedef struct {
	uint8_t package_digest[32]; /* TSAP 包整文件 sha256（须与设备计算值相等） */
	char permissions[ENTELEA_TRIAL_PERM_MAX]; /* 申请权限列表（同 request） */
	uint8_t permissions_len;
	uint8_t runtime_version;    /* Wasm/Lua 载荷版本 */
	uint8_t payload_kind;       /* 0=wasm 1=lua */
	uint32_t budget_steps;      /* 执行预算（工厂域 1..10^7） */
	uint32_t mem_quota_kb;      /* 内存配额（工厂域 1..512） */
	char description[48];       /* 候选说明（用户读） */
} entelea_factory_candidate_t;

/** 固件侧对工厂产物的验收面：slot 上的包经设备验签并计算整文件摘要后，
 * 须与 package_digest 相等（否则 E_INVALID_SIG，不提交）；权限列表/预算
 * 域外拒；payload_kind 未知拒（不默认解释）；slot 域外=E_PARAM。只到
 * PENDING——不代用户确认。预算与权限未与签名 manifest 对拍（D-D：首期
 * 不入签，由用户确认与持久记录约束）。 */
entelea_result_t entelea_factory_intake(const entelea_factory_candidate_t *fc,
					uint8_t slot);

/* ---- 跨重启持久（板级批 A-4；经 R03 meta 双副本） ---- */

/** [thread] 状态落盘（含 NONE 态——清槽也持久）；两副本分别写入
 * 并校验一致，任一步失败报错。调用时机归装配层；磨损策略随 D05。 */
entelea_result_t entelea_trial_flush(void);

/** [boot] 恢复：两副本均有效且内容一致才接受；全逻辑区可读且
 * 全擦除视为无记录；否则 E_IO、TIMEOUT + UNKNOWN（两槽均受门拒）。
 * 已初始化时幂等不覆盖内存状态；UNKNOWN 时可显式重试读取。
 * 首次 submit/执行门/flush 也会先恢复；普通 submit 不解除 UNKNOWN，
 * 维护恢复方式尚待确定。写入后须显式 flush。 */
entelea_result_t entelea_trial_restore(void);

#ifdef ENTELEA_TEST
void entelea_trial_test_reset(void);
/* 状态机测试接缝：直接接受调用方摘要且不验签，只供状态机用例使用；
 * 生产路径（wasm 套件）以签名包验证设备计算的摘要。字段校验与 submit 一致。 */
entelea_result_t entelea_trial_submit_with_digest_for_test(
	const entelea_trial_candidate_t *cand);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_DELIVERY_H */
