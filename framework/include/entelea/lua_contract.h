/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LUA_CONTRACT_H
#define ENTELEA_LUA_CONTRACT_H

/* D03 Lua integration contract (R05 key feasibility-validation surface; formal
 * runtime support is delivered in a later batch). The mechanism contract is in
 * framework/services/package/contract.md (section: Lua integration). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_LUA_CONTRACT_CBOR_MAX 64

typedef enum {
	ENTELEA_LUA_FMT_BINARY =
		1, /* "\x1bLua" + bytecode version byte (5.3=0x53/5.4=0x54) */
} entelea_lua_fmt_t;

typedef struct {
	entelea_lua_fmt_t format;
	uint8_t version; /* bytecode version byte */
} entelea_lua_payload_info_t;

/** Identifies payload and version at byte level. A non-Lua payload returns
 * E_INVALID_SIG; it must not default to Wasm or infer permissions from the file
 * suffix. */
entelea_result_t
entelea_lua_payload_ident(const uint8_t *data, uint32_t len,
			  entelea_lua_payload_info_t *out);

/* ---- Memory budget (lua_Allocator hook semantics, per instance) ---- */

typedef struct {
	uint32_t quota_bytes;
	uint32_t used_bytes;
	uint32_t peak_bytes;
	uint32_t denied; /* over-quota denials (visible on VM OOM path) */
} entelea_lua_mem_budget_t;

entelea_result_t
entelea_lua_mem_init(entelea_lua_mem_budget_t *b, uint32_t quota_bytes);
/** alloc/realloc/free hook (old_n -> new_n; shrinking always succeeds). Over
 * quota returns false and increments the denial count. */
bool
entelea_lua_mem_alloc(entelea_lua_mem_budget_t *b, uint32_t old_n,
		      uint32_t new_n);

/* ---- Execution budget and bounded stop (sethook count semantics) ---- */

typedef struct {
	uint32_t step_budget;
	uint32_t steps_used;
	bool aborted;
} entelea_lua_exec_budget_t;

entelea_result_t
entelea_lua_exec_init(entelea_lua_exec_budget_t *b, uint32_t step_budget);
/** Per-step gate: over budget returns false, and every later step is refused
 * (a bounded stop). The caller must return E_TIMEOUT and must not silently
 * continue. A deadline does not replace the execution budget (R01 app.h
 * semantics). */
bool
entelea_lua_exec_step(entelea_lua_exec_budget_t *b);

/* ---- Factory artifact description (R12 Forge comparison surface) ---- */

typedef struct {
	uint8_t version; /* contract version (=1) */
	uint16_t mem_quota_kb; /* memory quota in KB (1..512) */
	uint32_t exec_budget_steps; /* execution step budget (1..1e7) */
	entelea_lua_fmt_t format;
} entelea_lua_contract_t;

/** Deterministic encoding, byte-for-byte identical to Forge build_manifest:
 * map(4){0:version, 1:mem_kb, 2:steps, 3:fmt(lua=1)}. Int keys are sorted
 * ascending, and integers use the shortest encoding. An out-of-domain or
 * unknown version or format returns E_PARAM and does not write out/out_len. */
entelea_result_t
entelea_lua_contract_encode(const entelea_lua_contract_t *c,
			    uint8_t out[ENTELEA_LUA_CONTRACT_CBOR_MAX],
			    uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_LUA_CONTRACT_H */
