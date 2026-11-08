/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_RESOURCE_H
#define ENTELEA_RESOURCE_H

/* R04 资源预算合同见 framework/execution/resource/contract.md。
 * 功率预算：总额来自 prov（运行时只读）；记账 = 各输出通道 safety
 * readback 之和（单源纪律，不另设影子账本）；超预算不在线降额。
 * 检查-提交原子化：预算检查与 commit 同一 safety write_lock 临界区
 * （复用 entelea_safety_commit_locked）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>
#include <entelea/safety.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t budget_ma;
	uint32_t used_ma;
	uint32_t peak_ma;
	uint16_t slots;
} entelea_power_snapshot_t;

/** [any] 预算总额（prov 只读；未配置=0=一切供电请求拒绝——缺省安全侧）。 */
uint32_t entelea_power_budget_ma(void);

/** [any] 当前用电（Σ 已启用通道 readback；顺带更新峰值）。 */
uint32_t entelea_power_used_ma(void);

/** [any] 快照（观测面）。 */
void entelea_power_snapshot(entelea_power_snapshot_t *out);

/** [thread] 受控供电提交（原子预算检查+提交）：
 * ① SET 门（principal 对 power:write 声明）；
 * ② safety write_lock 内：预算检查（Σ+请求 ≤ 预算）→
 *    entelea_safety_commit_locked（保护末段仍在其内）→ 解锁。
 * 超预算 = E_RANGE（整笔拒绝，既有通道不受影响）；通道须 ACTIVE。
 * v.en=false（关闭）不受预算限。 */
entelea_result_t entelea_power_request(entelea_principal_t ctx,
				       const char *safety_uid,
				       entelea_out_value_t v);

#ifdef ENTELEA_TEST
void entelea_power_test_set_budget(uint32_t budget_ma);
void entelea_power_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_RESOURCE_H */
