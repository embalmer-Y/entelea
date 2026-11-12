/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_WASM_HOST_H
#define ENTELEA_WASM_HOST_H

/* R05 Wasm 执行闭环合同见 framework/services/package/contract.md（§Wasm
 * 宿主）。WAMR pinned 25bd7eb；fast 解释器 + 指令配额 + WASI 关 +
 * 池模式（旧 DEC-17/25/31/48③ 语义）。实例在 load 时绑定 R04 owner
 * principal，此后不可替换：natives 只按 owner 裁决（Wasm 传入整数不可转
 * principal）；owner={0} 未绑定 = natives 一律拒）。natives 授权取交集：
 * 试用确认的权限列表（仅 ACTIVE 且字节相符）与 owner 的宿主授权须同时允许；
 * 未经确认的包（NONE/非候选）无任何 natives 权限。执行配额 = min(请求,
 * 确认步数, 1e8)。 */

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
#define ENTELEA_WASM_INSTRUCTIONS_PER_MS 1000U
#define ENTELEA_WASM_DEFAULT_INSTRUCTIONS 100000000U

/** [boot] WAMR 进程级单次 init（池模式注入）。重复调用幂等。natives 注册
 * 失败 = E_IO 且回滚运行时（保持未初始化，可重试）。 */
entelea_result_t
entelea_wasm_init(void);

/** 验签并装载 slot 的模块，绑定 owner（V1 单实例）。已有活实例 =
 * E_BUSY 且零副作用（先 stop）；验签/读错误原样传播（E_INVALID_SIG /
 * E_IO 不互相改写）；R12 试用门（整文件 sha256）拒 = E_PERM 且零副作用；
 * 模块超装载缓冲 = E_RANGE。失败不写 app_id_out。 */
entelea_result_t
entelea_wasm_load(uint8_t slot, entelea_principal_t owner,
		  uint32_t *app_id_out);

/** 调用导出函数 fn。ctx 必须等于 load 绑定的 owner，否则 E_PERM；试用门
 * 按装载时 slot+摘要复核，拒 = E_PERM（之后 D12 门）。导出
 * ABI 仅 ()->i32 与 (i32)->i32：前者 arg 必须为 0，后者收到 arg；其余
 * 签名 = E_PARAM。上述拒绝均不执行、不销毁实例。超时 ms 经步预算近似，
 * 到期 → E_TIMEOUT 有界停止（判别=WAMR 异常串 "instruction limit"）；
 * 其余 trap → E_IO；两者都销毁实例。预算按 1000 指令/ms 近似换算，
 * 封顶 1e8；显式传 0 表示零指令，不是默认值，也不表示实际墙钟期限。 */
entelea_result_t
entelea_wasm_call(uint32_t app_id, entelea_principal_t ctx, const char *fn,
		  int32_t arg, uint32_t timeout_ms, int32_t *ret_out);

/** 使用默认 1e8 指令预算调用；其余语义与 entelea_wasm_call 相同。 */
entelea_result_t
entelea_wasm_call_default(uint32_t app_id, entelea_principal_t ctx,
			  const char *fn, int32_t arg, int32_t *ret_out);

/** 停止并销毁实例（模块保留——WAMR 换包重启路径语义）。 */
entelea_result_t
entelea_wasm_stop(uint32_t app_id);

/** 已装载实例数（观测）。 */
uint8_t
entelea_wasm_instance_count(void);

#ifdef ENTELEA_TEST
/* 停止实例、卸载模块并销毁运行时（下次 init 从零开始）。 */
void
entelea_wasm_test_reset(void);
/* 代表 native 裁决直达钩子：grant=确认权限列表（NULL = 无确认授权）。 */
int32_t
entelea_wasm_test_probe_decide(entelea_principal_t owner, const char *grant);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_WASM_HOST_H */
