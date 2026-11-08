/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_WASM_HOST_H
#define ENTELEA_WASM_HOST_H

/* R05 Wasm 执行闭环合同见 framework/services/package/contract.md（§Wasm
 * 宿主）。WAMR pinned 25bd7eb；fast 解释器 + 指令配额 + WASI 关 +
 * 池模式（旧 DEC-17/25/31/48③ 语义）。实例与 R04 principal 绑定：
 * natives 只信宿主注入身份（Wasm 传入整数不可转 principal）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_WASM_HEAP_SIZE 262144U /* 池（旧 DEC-27 256KB 基线） */
#define ENTELEA_WASM_STACK_SIZE 4096U

/** [boot] WAMR 进程级单次 init（池模式注入）。重复调用幂等。 */
entelea_result_t entelea_wasm_init(void);

/** 装载已验证 slot 的模块（调用方先经 entelea_pkg_verify；本层不重复
 * 验签——双层纪律中本层信任包事实层）。 */
entelea_result_t entelea_wasm_load(uint8_t slot, uint32_t *app_id_out);

/** 实例化 + 调用导出函数（fn 名；超时 ms=步预算到期 → E_TIMEOUT 有界
 * 停止——实例销毁，无静默续跑）。principal 注入实例（natives 裁决用）。 */
entelea_result_t entelea_wasm_call(uint32_t app_id, entelea_principal_t ctx,
				   const char *fn, uint32_t timeout_ms,
				   int32_t *ret_out);

/** 停止并销毁实例（模块保留——WAMR 换包重启路径语义）。 */
entelea_result_t entelea_wasm_stop(uint32_t app_id);

/** 已装载实例数（观测）。 */
uint8_t entelea_wasm_instance_count(void);

#ifdef ENTELEA_TEST
void entelea_wasm_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_WASM_HOST_H */
