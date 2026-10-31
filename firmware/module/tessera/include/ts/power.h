/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ts-power 公共 API（design/LLD-ts-power.md）：受控供电——供电槽开关、
 * 限流、功率预算（DEC-03：供电视同输出，走同一安全合同）。
 * 合同关联：合同 7（纳入 1-6 同一合同）、1（供电通道三安全态必声明）、
 * 2（开关必经 ts_safety_commit 唯一出口——本模块零直接驱动调用）。
 */
#ifndef TS_POWER_H__
#define TS_POWER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ts/err.h>
#include <ts/hal.h> /* ts_ctx_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 供电槽（LLD §2）----------------------------------------------------- */

typedef struct {
	const char *uid;            /* 如 "pwr0"；注册为 TS_CH_POWER 通道 */
	uint32_t current_limit_ma;  /* 硬限流（注册期冻结进 limits，合同 10） */
	bool poweron_on;            /* 上电态是否供电（ma 取 current_limit_ma——
				     * 保守上电口径，计入预算） */
} ts_pwr_slot_t;

/** 注册供电槽（init 期 / ts-periph 描述符链驱动）：
 * 三安全态由此模块按合同 1 生成（poweron = 声明值；linkloss/fault = 关断——
 * 供电安全侧缺省，不接受调用方放宽）。注册期冻结，运行时只读。
 * 生命周期契约：槽结构体被拷入模块静态记录（允许栈上组装）；uid 字符串
 * 须指向静态存储（通道注册表持引用）。 */
ts_res_t ts_power_register_slot(const ts_pwr_slot_t *s);

size_t ts_power_slot_count(void);
const ts_pwr_slot_t *ts_power_slot_get(size_t idx);

/** 供电请求（ts-hal/APP 面；权限裁决 TS_PERM_CLASS_POWER/SET 在此）。
 * 流程：预算检查（§3，超 → TS_E_RANGE + TS_EVT_POWER_BUDGET）→
 * ts_safety_commit(uid, {.pwr={on, ma}})——物理生效唯一路径（合同 2/7）；
 * 限流由通道 limits.current_limit_ma 兜底（越限 → TS_E_RANGE）。 */
ts_res_t ts_power_request(ts_ctx_t c, uint8_t slot, bool on, uint32_t ma);

/* ---- 功率预算（LLD §3）--------------------------------------------------- */

typedef struct {
	uint32_t budget_ma; /* 总额：prov 只读（板级烧录，合同 10；缺省 0 = 拒绝） */
	uint32_t used_ma;   /* 活跃槽 Σ(ma)（读回权威——不在线降额） */
	uint32_t peak_ma;   /* 历史峰值 */
	uint8_t slots;      /* 注册槽数 */
} ts_power_budget_t;

/** 快照（sys get-budget / 遥测 kind 97 消费）。 */
void ts_power_budget_snapshot(ts_power_budget_t *out);

#ifdef CONFIG_TS_TEST
/* 测试注入：预算总额与记账清零（prod 无此通道——prov 是唯一预算源） */
void ts_power_test_set_budget(uint32_t budget_ma);
void ts_power_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TS_POWER_H__ */
