/* SPDX-License-Identifier: Apache-2.0 */
/*
 * D03 Lua integration contract: key feasibility validation (R05; see
 * contract.md in this directory).
 * Goals (plan wording): payload/version identification, host calls and
 * trusted identity, memory budget, execution budget with bounded interrupt
 * and stop, errors/receipts, data isolation, and factory artifact
 * description. Representative validation checks resource and stop
 * constraints. Validation may find platforms or capabilities that do not
 * apply; these must be recorded explicitly.
 *
 * This file is mechanism validation with no Lua interpreter dependency
 * (no new dependency has been approved):
 *  - Payload/version identification: Lua header "\x1bLua" plus version
 *    0x53/0x54 (5.3/5.4), byte-level (cross-checked against a subset of
 *    luaL_checkheader semantics).
 *  - Trusted identity: natives trust only the principal injected by the host
 *    (R04 surface; integers input inside the VM cannot become a principal,
 *    the same discipline as Wasm).
 *  - Memory budget: per-instance quota plus an allocation gate (over-quota
 *    requests are refused; this stands in for the lua_Allocator hook point,
 *    to be attached to the allocator when a real interpreter is integrated).
 *  - Execution budget and bounded stop: instruction-count gate (stands in for
 *    lua_sethook count-mode budget semantics: over limit means stop and
 *    return an error, never silently continue).
 *  - Data isolation: memory is not shared between instances (independent
 *    quota and no shared blob surface).
 *  - Factory artifact description: integration contract struct (for R12 Forge
 *    generation and cross-check).
 *
 * Honest boundary: no Lua interpreter is installed, and the stop latency of a
 * real Lua VM is not proven (hook point semantics are fixed; measurement is
 * deferred to the formal support batch). This validation proves the
 * **mechanism feasibility of the integration contract**.
 */
#include <string.h>
#include <entelea/lua_contract.h>

/* ---- Payload/version identification (byte level, fail-closed) ---- */

entelea_result_t
entelea_lua_payload_ident(const uint8_t *data, uint32_t len,
			  entelea_lua_payload_info_t *out)
{
	if (data == NULL || out == NULL || len < 4) {
		return ENTELEA_E_PARAM;
	}
	if (data[0] != 0x1b || data[1] != 'L' || data[2] != 'u' ||
	    data[3] != 'a') {
		return ENTELEA_E_INVALID_SIG; /* non-Lua payload (fail-closed;
					       * must not default to Wasm or
					       * infer from suffix). */
	}
	if (len < 5) {
		return ENTELEA_E_INVALID_SIG;
	}
	out->format = ENTELEA_LUA_FMT_BINARY;
	out->version = data[4]; /* 0x53=5.3 / 0x54=5.4 / 0x55=5.5 (bytecode) */
	return ENTELEA_OK;
}

/* ---- Memory budget (allocation gate; lua_Allocator hook semantics) ---- */

entelea_result_t
entelea_lua_mem_init(entelea_lua_mem_budget_t *b, uint32_t quota_bytes)
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

/* Stand-in for the allocator's alloc/realloc/free hooks: over-quota
 * requests are refused (NULL) and counted. Instance isolation: each
 * instance has its own budget (no shared-pool bypass). */
bool
entelea_lua_mem_alloc(entelea_lua_mem_budget_t *b, uint32_t old_n,
		      uint32_t new_n)
{
	if (b == NULL) {
		return false;
	}
	if (new_n > old_n) {
		uint32_t grow = new_n - old_n;

		if (grow > b->quota_bytes - b->used_bytes) {
			b->denied++; /* Over quota: refuse (VM OOM path) */
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

/* ---- Execution budget and bounded stop (sethook count gate) ---- */

entelea_result_t
entelea_lua_exec_init(entelea_lua_exec_budget_t *b, uint32_t step_budget)
{
	if (b == NULL || step_budget == 0) {
		return ENTELEA_E_PARAM;
	}
	b->step_budget = step_budget;
	b->steps_used = 0;
	b->aborted = false;
	return ENTELEA_OK;
}

/* Called per step. Over budget returns false: the caller must stop and
 * return E_TIMEOUT, never silently continue. For deadline semantics see
 * R01 app.h: a deadline does not replace the execution budget. */
bool
entelea_lua_exec_step(entelea_lua_exec_budget_t *b)
{
	if (b == NULL || b->aborted) {
		return false;
	}
	if (b->steps_used >= b->step_budget) {
		b->aborted = true; /* Bounded stop: all later steps refused */
		return false;
	}
	b->steps_used++;
	return true;
}

/* ---- Factory artifact description (contract struct for R12 Forge) ---- */

/* CBOR unsigned integer (RFC 8949 preferred serialization: shortest form). */
static uint32_t
put_uint(uint8_t *out, uint32_t v)
{
	if (v < 24U) {
		out[0] = (uint8_t) v;
		return 1;
	}
	if (v < 256U) {
		out[0] = 0x18;
		out[1] = (uint8_t) v;
		return 2;
	}
	if (v < 65536U) {
		out[0] = 0x19;
		out[1] = (uint8_t) (v >> 8);
		out[2] = (uint8_t) v;
		return 3;
	}
	out[0] = 0x1a;
	out[1] = (uint8_t) (v >> 24);
	out[2] = (uint8_t) (v >> 16);
	out[3] = (uint8_t) (v >> 8);
	out[4] = (uint8_t) v;
	return 5;
}

/* Byte-for-byte identical to the Forge packer build_manifest:
 * map(4) {0:version, 1:mem_kb, 2:steps, 3:fmt}, int keys in ascending
 * numeric order (RFC 8949 deterministic encoding). */
entelea_result_t
entelea_lua_contract_encode(const entelea_lua_contract_t *c,
			    uint8_t out[ENTELEA_LUA_CONTRACT_CBOR_MAX],
			    uint32_t *out_len)
{
	if (c == NULL || out == NULL || out_len == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (c->version != 1 || c->format != ENTELEA_LUA_FMT_BINARY) {
		return ENTELEA_E_PARAM; /* Bad version/format: fail-closed */
	}
	if (c->mem_quota_kb == 0 || c->mem_quota_kb > 512 ||
	    c->exec_budget_steps == 0 || c->exec_budget_steps > 10000000U) {
		return ENTELEA_E_PARAM; /* out of domain: suggested limits are
					 * 512KB memory, 1e7 steps (factory) */
	}
	const uint32_t val[4] = { c->version, c->mem_quota_kb,
				  c->exec_budget_steps, (uint32_t) c->format };
	uint32_t n = 0;

	out[n++] = 0xa4;
	for (uint32_t k = 0; k < 4; k++) {
		out[n++] = (uint8_t) k;
		n += put_uint(out + n, val[k]);
	}
	*out_len = n; /* Max 1(map)+4(keys)+1+3+5+1 = 15 <= CBOR_MAX */
	return ENTELEA_OK;
}
