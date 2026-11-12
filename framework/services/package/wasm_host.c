/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wasm execution closed-loop host (R05; migrated from the minimal closed-loop
 * subset of the old appmgr/runtime.c; see framework/services/package/
 * contract.md, section "Wasm host").
 * WAMR build surface (CMake consumer wiring is in tests/wasm/CMakeLists.txt):
 * fast interpreter + INSTRUCTION_METERING (step budget) + WASI off + pool mode
 * (old DEC-17/25/31/48 item 3).
 * Minimal loop: load (verified slot + R12 trial gate; the owner binds at this
 * point) -> call (owner only; trial gate re-checked; exported ABI allowlist;
 * step budget expiry = E_TIMEOUT, a bounded stop) -> stop (instance destroyed,
 * module kept; the package-replacement restart path).
 * natives: this minimal loop registers one representative native
 * ("entelea"."host_probe" (i)i, decided by the bound owner); the full symbol
 * set expands with the R06+ consumer surfaces.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/appstore.h>
#include <entelea/guard.h>
#include <entelea/delivery.h>
#include <entelea/wasm_host.h>
#include <wasm_export.h>

#define MAX_INST 1 /* V1 single instance (old DEC-31 semantics) */

struct inst {
	bool used;
	uint32_t app_id;
	wasm_module_inst_t inst;
	wasm_exec_env_t env;
	entelea_principal_t owner;
	uint8_t slot; /* Load source (trial gate recheck target) */
	uint8_t digest[32]; /* sha256 of the whole file at load */
	bool grant_ok; /* Grant valid for this call (ACTIVE, bytes match) */
	entelea_perm_table_t grant; /* Permissions (valid if grant_ok) */
	uint32_t budget_steps; /* Step limit: confirmed, else device max */
};
static struct inst insts[MAX_INST];
static bool wamr_inited;
static uint8_t wamr_heap[ENTELEA_WASM_HEAP_SIZE] __aligned(8);

/* Representative native: decided by the host-injected principal
 * (gpio:read:0 gate), proving natives trust only injected identity.
 * Integers passed from Wasm are ignored. */
/* D-B intersection: both the confirmed permission list and the
 * host-injected principal grant must allow the access. */
static bool
app_allows(const struct inst *it, entelea_perm_class_t cls,
	   entelea_perm_op_t op, uint8_t inst)
{
	if (it == NULL || !it->grant_ok) {
		return false;
	}
	if ((it->grant.grant[cls][op] & (1U << inst)) == 0U) {
		return false;
	}
	return entelea_perm_check(it->owner, cls, op, inst) == ENTELEA_OK;
}

static int32_t
probe_decide(const struct inst *it, uint32_t ctx_opaque_ignored)
{
	(void) ctx_opaque_ignored;
	if (!app_allows(it, ENTELEA_PERM_GPIO, ENTELEA_PERM_READ, 0)) {
		return ENTELEA_E_PERM;
	}
	return 1; /* Grant visible */
}

static int32_t
native_host_probe(wasm_exec_env_t env, uint32_t ctx_opaque_ignored)
{
	struct inst *it = (struct inst *) wasm_runtime_get_user_data(env);

	return probe_decide(it, ctx_opaque_ignored);
}

#ifdef ENTELEA_TEST
/* Direct hook into the decision function, for inputs that are hard to
 * construct through load (forged or invalid principal, missing confirmed
 * permissions). grant=NULL means no confirmed permissions. The real
 * VM-to-native path is covered by tests/wasm. */
int32_t
entelea_wasm_test_probe_decide(entelea_principal_t owner, const char *grant)
{
	struct inst fake;

	memset(&fake, 0, sizeof(fake));
	fake.owner = owner;
	if (grant != NULL) {
		entelea_perm_table_init(&fake.grant);
		if (entelea_perm_parse_list(grant, strlen(grant),
					    &fake.grant) != ENTELEA_OK) {
			return ENTELEA_E_PARAM;
		}
		fake.grant_ok = true;
	}
	return probe_decide(&fake, 0);
}
#endif

static NativeSymbol native_syms[] = {
	{
		"host_probe",
		(void *) native_host_probe,
		"(i)i",
		NULL,
	},
};

entelea_result_t
entelea_wasm_init(void)
{
	if (wamr_inited) {
		return ENTELEA_OK;
	}
	RuntimeInitArgs args;

	memset(&args, 0, sizeof(args));
	args.mem_alloc_type = Alloc_With_Pool;
	args.mem_alloc_option.pool.heap_buf = wamr_heap;
	args.mem_alloc_option.pool.heap_size = sizeof(wamr_heap);
	if (!wasm_runtime_full_init(&args)) {
		return ENTELEA_E_IO;
	}
	/* RF1: returns bool (the original `< 0` check was always false and
	 * swallowed failures). A host without natives must not report OK:
	 * roll back the runtime so the uninitialized state can be retried. */
	if (!wasm_runtime_register_natives("entelea", native_syms,
					   sizeof(native_syms) /
						   sizeof(native_syms[0]))) {
		wasm_runtime_destroy();
		return ENTELEA_E_IO;
	}
	wamr_inited = true;
	return ENTELEA_OK;
}

/* Load buffer: WAMR requires it writable and alive until unload. Overwrite
 * it only when no module references it. A single-pass capture
 * (store.c entelea_pkg_file_digest_capture) writes here. */
static uint8_t load_buf[ENTELEA_APP_SLOT_SIZE / 4]; /* 64KB load limit */

static struct inst *
find_free(void)
{
	for (int i = 0; i < MAX_INST; i++) {
		if (!insts[i].used) {
			return &insts[i];
		}
	}
	return NULL;
}

static struct inst *
find_app(uint32_t app_id)
{
	for (int i = 0; i < MAX_INST; i++) {
		if (insts[i].used && insts[i].app_id == app_id) {
			return &insts[i];
		}
	}
	return NULL;
}

/* The only module (V1 single instance): the live instance references it and
 * the load buffer; neither may be unloaded or overwritten while alive. */
static wasm_module_t module;

static bool
same_principal(entelea_principal_t a, entelea_principal_t b)
{
	return a._handle == b._handle && a._gen == b._gen &&
	       a.app_id == b.app_id;
}

/* Exported ABI allowlist ()->i32 | (i32)->i32: returns the parameter count;
 * mismatch = -1. */
static int
abi_params(wasm_function_inst_t f, wasm_module_inst_t mi)
{
	wasm_valkind_t k;

	if (wasm_func_get_result_count(f, mi) != 1) {
		return -1;
	}
	wasm_func_get_result_types(f, mi, &k);
	if (k != WASM_I32) {
		return -1;
	}
	uint32_t pc = wasm_func_get_param_count(f, mi);

	if (pc == 0) {
		return 0;
	}
	if (pc != 1) {
		return -1;
	}
	wasm_func_get_param_types(f, mi, &k);
	return k == WASM_I32 ? 1 : -1;
}

entelea_result_t
entelea_wasm_load(uint8_t slot, entelea_principal_t owner, uint32_t *app_id_out)
{
	if (app_id_out == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t rc = entelea_wasm_init();

	if (rc != ENTELEA_OK) {
		return rc;
	}
	/* RF1 P1-7: refuse first when occupied, with no side effects (the
	 * old code unloaded the module before checking for a free slot,
	 * leaving a live instance pointing at a freed module). */
	struct inst *it = find_free();

	if (it == NULL || entelea_wasm_instance_count() != 0) {
		return ENTELEA_E_BUSY;
	}
	entelea_tsap_view_t v;

	rc = entelea_pkg_verify(slot, NULL, &v, NULL);
	if (rc != ENTELEA_OK) {
		return rc; /* RF1: propagate (media E_IO != bad package) */
	}
	if (v.wasm_len > sizeof(load_buf)) {
		return ENTELEA_E_RANGE; /* 64KB load limit (no side effects) */
	}
	/* RF1 #9: R12 trial gate. The candidate slot may only load the bytes
	 * the user confirmed (digest domain = Forge package_digest domain).
	 * Refusal comes before unloading the old module (no side effects). */
	uint8_t dg[32];
	uint8_t dg_load[32];

	rc = entelea_pkg_file_digest(slot, v.pkg_len, dg);
	if (rc != ENTELEA_OK) {
		return rc;
	}
	rc = entelea_trial_gate(slot, dg);
	if (rc != ENTELEA_OK) {
		return rc;
	}
	/* Each load unloads and rebuilds: after re-staging the same slot,
	 * the old cache must not revive (the verified and executed objects
	 * are both the new bytes). Unload first, then overwrite the load
	 * buffer. */
	if (module != NULL) {
		wasm_runtime_unload(module);
		module = NULL;
	}
	/* F3: the loaded bytes and the gate digest come from one read pass;
	 * a mismatch refuses the load (module already unloaded, no instance
	 * created). */
	rc = entelea_pkg_file_digest_capture(slot, v.pkg_len, dg_load,
					     v.wasm_off, v.wasm_len, load_buf);
	if (rc != ENTELEA_OK) {
		return rc;
	}
	if (memcmp(dg, dg_load, sizeof(dg)) != 0) {
		return ENTELEA_E_PERM;
	}
	char err[128];

	module = wasm_runtime_load(load_buf, v.wasm_len, err, sizeof(err));
	if (module == NULL) {
		return ENTELEA_E_IO;
	}
	it->inst = wasm_runtime_instantiate(module, ENTELEA_WASM_STACK_SIZE,
					    8192, err, sizeof(err));
	if (it->inst == NULL) {
		return ENTELEA_E_IO;
	}
	it->env =
		wasm_runtime_create_exec_env(it->inst, ENTELEA_WASM_STACK_SIZE);
	if (it->env == NULL) {
		wasm_runtime_deinstantiate(it->inst);
		it->inst = NULL;
		return ENTELEA_E_IO;
	}
	it->used = true;
	it->app_id = (uint32_t) (it - insts) + 1;
	it->owner = owner; /* RF1 P1-8: identity bound at load */
	it->slot = slot;
	memcpy(it->digest, dg, sizeof(it->digest));
	it->grant_ok = false;
	entelea_perm_table_init(&it->grant);
	it->budget_steps = ENTELEA_WASM_DEFAULT_INSTRUCTIONS;
	wasm_runtime_set_user_data(it->env, it);
	*app_id_out = it->app_id;
	return ENTELEA_OK;
}

entelea_result_t
entelea_wasm_call(uint32_t app_id, entelea_principal_t ctx, const char *fn,
		  int32_t arg, uint32_t timeout_ms, int32_t *ret_out)
{
	struct inst *it = find_app(app_id);

	if (it == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	if (fn == NULL) {
		return ENTELEA_E_PARAM;
	}
	/* RF1 P1-8: only the bound owner may call (refuse; no teardown) */
	if (!same_principal(ctx, it->owner)) {
		return ENTELEA_E_PERM;
	}
	{ /* RF1 #9: trial gate recheck; a new candidate in the same slot
	   * after load is E_PERM (no execution, no teardown). */
		entelea_result_t tg = entelea_trial_gate(it->slot, it->digest);

		if (tg != ENTELEA_OK) {
			return tg;
		}
	}
	{ /* R12 grant lookup (per call, not cached; D-B/D-C/D-E):
	   * granted when the trial is ACTIVE and bytes match, or the slot's
	   * stable record digest matches. Grants permissions and steps;
	   * anything else gets no natives permissions (A1) and the
	   * device-maximum quota. */
		entelea_trial_grant_t g;
		entelea_result_t gr =
			entelea_trial_grant(it->slot, it->digest, &g);

		if (gr != ENTELEA_OK) {
			gr = entelea_stable_grant(it->slot, it->digest, &g);
		}

		entelea_perm_table_init(&it->grant);
		it->grant_ok = false;
		it->budget_steps = ENTELEA_WASM_DEFAULT_INSTRUCTIONS;
		if (gr == ENTELEA_OK &&
		    entelea_perm_parse_list(g.perm, g.perm_len, &it->grant) ==
			    ENTELEA_OK) {
			it->grant_ok = true;
			it->budget_steps = g.budget_steps;
		} else {
			entelea_perm_table_init(&it->grant);
		}
	}
	{ /* D12 cooldown precheck (60s after abnormal reset; E_BUSY) */
		entelea_result_t gate = entelea_guard_wasm_allow();

		if (gate != ENTELEA_OK) {
			return gate;
		}
	}
	wasm_function_inst_t f = wasm_runtime_lookup_function(it->inst, fn);

	if (f == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	/* RF1 P1-8: a signature outside the allowlist, or a non-zero arg to
	 * a no-arg function, is E_PARAM (no execution, no teardown; the old
	 * hard-coded argv={0} silently dropped the argument). */
	int np = abi_params(f, it->inst);

	if (np < 0 || (np == 0 && arg != 0)) {
		return ENTELEA_E_PARAM;
	}
	uint32_t argv[1] = { (uint32_t) arg };

	/* Bounded execution: timeout_ms is approximated by the step-budget
	 * instruction quota (1000 instructions/ms; DEC-48 item 3 semantics).
	 * Expiry = WAMR quota exception = E_TIMEOUT, a bounded stop. The WAMR
	 * parameter is int, capped at 1e8 (D12 owner v1 quota), far below
	 * INT_MAX, so it cannot truncate to a negative value. timeout_ms=0 is
	 * a zero quota (expires at the first instruction), not "default or
	 * unbounded". */
	uint32_t quota = timeout_ms;

	if (quota > ENTELEA_WASM_DEFAULT_INSTRUCTIONS /
			    ENTELEA_WASM_INSTRUCTIONS_PER_MS) {
		quota = ENTELEA_WASM_DEFAULT_INSTRUCTIONS /
			ENTELEA_WASM_INSTRUCTIONS_PER_MS;
	}
	uint32_t limit = quota * ENTELEA_WASM_INSTRUCTIONS_PER_MS;

	if (limit > it->budget_steps) {
		limit = it->budget_steps; /* D-C: min(req, confirmed, cap) */
	}
	wasm_runtime_set_instruction_count_limit(it->env, (int) limit);
	bool ok = wasm_runtime_call_wasm(it->env, f, (uint32_t) np, argv);

	if (!ok) {
		/* Exception path (E-2 narrowed): quota expiry is not a trap.
		 * Expiry = E_TIMEOUT (bounded stop, DEC-48 item 3); any other
		 * trap = E_IO. Neither instance is trusted, so both are
		 * stopped and destroyed. Discrimination uses the WAMR
		 * exception string (fast and classic interpreters share
		 * "instruction limit exceeded"; pinned at 25bd7eb). */
		const char *ex = wasm_runtime_get_exception(it->inst);
		bool quota =
			ex != NULL && strstr(ex, "instruction limit") != NULL;

		wasm_runtime_clear_exception(it->inst);
		entelea_wasm_stop(app_id);
		return quota ? ENTELEA_E_TIMEOUT : ENTELEA_E_IO;
	}
	if (ret_out != NULL) {
		*ret_out = (int32_t) argv[0];
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_wasm_call_default(uint32_t app_id, entelea_principal_t ctx,
			  const char *fn, int32_t arg, int32_t *ret_out)
{
	return entelea_wasm_call(app_id, ctx, fn, arg,
				 ENTELEA_WASM_DEFAULT_INSTRUCTIONS /
					 ENTELEA_WASM_INSTRUCTIONS_PER_MS,
				 ret_out);
}

entelea_result_t
entelea_wasm_stop(uint32_t app_id)
{
	struct inst *it = find_app(app_id);

	if (it == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	if (it->env != NULL) {
		wasm_runtime_destroy_exec_env(it->env);
	}
	if (it->inst != NULL) {
		wasm_runtime_deinstantiate(it->inst);
	}
	it->used = false;
	it->env = NULL;
	it->inst = NULL;
	it->app_id = 0;
	return ENTELEA_OK;
}

uint8_t
entelea_wasm_instance_count(void)
{
	uint8_t n = 0;

	for (int i = 0; i < MAX_INST; i++) {
		if (insts[i].used) {
			n++;
		}
	}
	return n;
}

#ifdef ENTELEA_TEST
void
entelea_wasm_test_reset(void)
{
	for (int i = 0; i < MAX_INST; i++) {
		if (insts[i].used) {
			entelea_wasm_stop(insts[i].app_id);
		}
	}
	if (module != NULL) {
		wasm_runtime_unload(module);
		module = NULL;
	}
	if (wamr_inited) { /* Runtime and natives reclaimed; re-init */
		wasm_runtime_destroy();
		wamr_inited = false;
	}
}
#endif
