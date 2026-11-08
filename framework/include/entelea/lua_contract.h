/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LUA_CONTRACT_H
#define ENTELEA_LUA_CONTRACT_H

/* D03 Lua 接入合同（R05 关键可行性验证面；正式运行支持后续批次交付）。
 * 机制合同见 framework/services/package/contract.md（§Lua 接入）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_LUA_CONTRACT_CBOR_MAX 64

typedef enum {
	ENTELEA_LUA_FMT_BINARY = 1, /* "\x1bLua"+ver 字节码（5.3=0x53/5.4=0x54） */
} entelea_lua_fmt_t;

typedef struct {
	entelea_lua_fmt_t format;
	uint8_t version; /* 字节码版本字节 */
} entelea_lua_payload_info_t;

/** 载荷/版本识别（字节级；非 Lua 载荷 = E_INVALID_SIG——不得默认解释为
 * Wasm 或按文件后缀推断权限）。 */
entelea_result_t entelea_lua_payload_ident(const uint8_t *data, uint32_t len,
					   entelea_lua_payload_info_t *out);

/* ---- 内存预算（lua_Allocator 挂点语义；每实例独立） ---- */

typedef struct {
	uint32_t quota_bytes;
	uint32_t used_bytes;
	uint32_t peak_bytes;
	uint32_t denied; /* 超配额拒绝次数（VM OOM 路径可观） */
} entelea_lua_mem_budget_t;

entelea_result_t entelea_lua_mem_init(entelea_lua_mem_budget_t *b,
				      uint32_t quota_bytes);
/** alloc/realloc/free 钩子（old_n→new_n；缩容恒过）。超配额=false+计数。 */
bool entelea_lua_mem_alloc(entelea_lua_mem_budget_t *b, uint32_t old_n,
			   uint32_t new_n);

/* ---- 执行预算与有界停止（sethook count 语义） ---- */

typedef struct {
	uint32_t step_budget;
	uint32_t steps_used;
	bool aborted;
} entelea_lua_exec_budget_t;

entelea_result_t entelea_lua_exec_init(entelea_lua_exec_budget_t *b,
				       uint32_t step_budget);
/** 每步闸：超预算=false（此后一切 step 拒——有界停止；调用方必须返回
 * E_TIMEOUT，不静默续跑。deadline 不替代执行预算——R01 app.h 语义）。 */
bool entelea_lua_exec_step(entelea_lua_exec_budget_t *b);

/* ---- 工厂产物描述（R12 Forge 对拍面） ---- */

typedef struct {
	uint8_t version;              /* 合同版本（=1） */
	uint16_t mem_quota_kb;        /* 内存配额（1..512） */
	uint32_t exec_budget_steps;   /* 执行步预算（1..1e7） */
	entelea_lua_fmt_t format;
} entelea_lua_contract_t;

/** 定序键确定性编码（map{version,mem_kb,steps,fmt}；域外=PARAM）。 */
entelea_result_t entelea_lua_contract_encode(
	const entelea_lua_contract_t *c,
	uint8_t out[ENTELEA_LUA_CONTRACT_CBOR_MAX], uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_LUA_CONTRACT_H */
