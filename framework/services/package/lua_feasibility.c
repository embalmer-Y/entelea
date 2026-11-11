/* SPDX-License-Identifier: Apache-2.0 */
/*
 * D03 Lua 接入合同——关键可行性验证（R05，合同见本目录 contract.md）。
 * 目标（规划原文）：载荷/版本识别、宿主调用与可信身份、内存预算、执行
 * 预算与有界中断/停止、错误/回执、数据隔离及工厂产物描述。用代表性验证
 * 检验资源与停止约束；验证可发现不适用的平台/能力，必须明确记录。
 *
 * 本文件 = 机制验证（无 Lua 解释器依赖——未批准新依赖）：
 *  - 载荷/版本识别：Lua 载荷头部 "\x1bLua" + version 0x53/0x54（5.3/5.4）
 *    字节识别（对拍 luaL_checkheader 语义子集）。
 *  - 可信身份：natives 只信宿主注入的 principal（R04 面；VM 内输入整数
 *    不可转 principal——与 Wasm 同纪律）。
 *  - 内存预算：每实例配额 + 分配闸（超配额拒——代表 lua_Allocator 的
 *    预算钩子点；真解释器接入时挂 allocator）。
 *  - 执行预算与有界停止：指令计数闸（代表 lua_sethook count 模式的
 *    预算语义：超限 → 停止并回错误，不静默续跑）。
 *  - 数据隔离：实例间内存不共享（配额独立 + 无共享 blob 面）。
 *  - 工厂产物描述：接入合同结构体（供 R12 Forge 生成/对拍）。
 *
 * 如实边界：不装 Lua 解释器、不证明真实 Lua VM 的停止延迟（挂点语义
 * 已定，实测归正式支持批次）；本验证证明的是**接入合同的机制可行性**。
 */
#include <string.h>
#include <entelea/lua_contract.h>

/* ---- 载荷/版本识别（字节级，fail-closed） ---- */

entelea_result_t entelea_lua_payload_ident(const uint8_t *data, uint32_t len,
					   entelea_lua_payload_info_t *out)
{
	if (data == NULL || out == NULL || len < 4) {
		return ENTELEA_E_PARAM;
	}
	if (data[0] != 0x1b || data[1] != 'L' || data[2] != 'u' ||
	    data[3] != 'a') {
		return ENTELEA_E_INVALID_SIG; /* 非 Lua 载荷（fail-closed，
						 * 不得默认解释为 Wasm 或按
						 * 后缀推断）。 */
	}
	if (len < 5) {
		return ENTELEA_E_INVALID_SIG;
	}
	out->format = ENTELEA_LUA_FMT_BINARY;
	out->version = data[4]; /* 0x53=5.3 / 0x54=5.4（字节码） */
	return ENTELEA_OK;
}

/* ---- 内存预算（分配闸——lua_Allocator 挂点语义） ---- */

entelea_result_t entelea_lua_mem_init(entelea_lua_mem_budget_t *b,
				      uint32_t quota_bytes)
{
	if (b == NULL || quota_bytes == 0) {
		return ENTELEA_E_PARAM;
	}
	b->quota_bytes = quota_bytes;
	b->used_bytes = 0;
	b->peak_bytes = 0;
	b->denied = 0;
	return ENTELEA_OK;
}

/* 代表 allocator 的 alloc/realloc/free 钩子：超配额 = 拒（NULL）+计数；
 * 实例隔离 = 每实例独立 budget（无共享池旁路）。 */
bool entelea_lua_mem_alloc(entelea_lua_mem_budget_t *b, uint32_t old_n,
			   uint32_t new_n)
{
	if (b == NULL) {
		return false;
	}
	if (new_n > old_n) {
		uint32_t grow = new_n - old_n;

		if (grow > b->quota_bytes - b->used_bytes) {
			b->denied++; /* 超配额：拒（VM 内 OOM 错误路径） */
			return false;
		}
		b->used_bytes += grow;
		if (b->used_bytes > b->peak_bytes) {
			b->peak_bytes = b->used_bytes;
		}
	} else {
		b->used_bytes -= old_n - new_n;
	}
	return true;
}

/* ---- 执行预算与有界停止（指令计数闸——sethook count 语义） ---- */

entelea_result_t entelea_lua_exec_init(entelea_lua_exec_budget_t *b,
				       uint32_t step_budget)
{
	if (b == NULL || step_budget == 0) {
		return ENTELEA_E_PARAM;
	}
	b->step_budget = step_budget;
	b->steps_used = 0;
	b->aborted = false;
	return ENTELEA_OK;
}

/* 每步调用：超预算 → false（调用方必须停止执行并返回 E_TIMEOUT——
 * 不静默续跑；deadline 语义见 R01 app.h：deadline 不替代执行预算）。 */
bool entelea_lua_exec_step(entelea_lua_exec_budget_t *b)
{
	if (b == NULL || b->aborted) {
		return false;
	}
	if (b->steps_used >= b->step_budget) {
		b->aborted = true; /* 有界停止：此后一切 step 拒 */
		return false;
	}
	b->steps_used++;
	return true;
}

/* ---- 工厂产物描述（接入合同结构体——R12 Forge 对拍面） ---- */

/* CBOR 无符号整数（RFC 8949 首选序列化：最短编码）。 */
static uint32_t put_uint(uint8_t *out, uint32_t v)
{
	if (v < 24U) {
		out[0] = (uint8_t)v;
		return 1;
	}
	if (v < 256U) {
		out[0] = 0x18;
		out[1] = (uint8_t)v;
		return 2;
	}
	if (v < 65536U) {
		out[0] = 0x19;
		out[1] = (uint8_t)(v >> 8);
		out[2] = (uint8_t)v;
		return 3;
	}
	out[0] = 0x1a;
	out[1] = (uint8_t)(v >> 24);
	out[2] = (uint8_t)(v >> 16);
	out[3] = (uint8_t)(v >> 8);
	out[4] = (uint8_t)v;
	return 5;
}

/* 与 Forge packer build_manifest 逐字节一致：map(4){0:version, 1:mem_kb,
 * 2:steps, 3:fmt}，int 键按数值升序（RFC 8949 确定性编码）。 */
entelea_result_t entelea_lua_contract_encode(
	const entelea_lua_contract_t *c, uint8_t out[ENTELEA_LUA_CONTRACT_CBOR_MAX],
	uint32_t *out_len)
{
	if (c == NULL || out == NULL || out_len == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (c->version != 1 || c->format != ENTELEA_LUA_FMT_BINARY) {
		return ENTELEA_E_PARAM; /* 未知合同版本/载荷格式 fail-closed */
	}
	if (c->mem_quota_kb == 0 || c->mem_quota_kb > 512 ||
	    c->exec_budget_steps == 0 ||
	    c->exec_budget_steps > 10000000U) {
		return ENTELEA_E_PARAM; /* 域外（建议上限：内存 512KB/步
					 * 1e7——工厂生成侧约束面） */
	}
	const uint32_t val[4] = {c->version, c->mem_quota_kb,
				 c->exec_budget_steps, (uint32_t)c->format};
	uint32_t n = 0;

	out[n++] = 0xa4;
	for (uint32_t k = 0; k < 4; k++) {
		out[n++] = (uint8_t)k;
		n += put_uint(out + n, val[k]);
	}
	*out_len = n; /* 上限 1(map)+4(键)+1+3+5+1 = 15 ≤ CBOR_MAX */
	return ENTELEA_OK;
}
