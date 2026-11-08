/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wasm 执行闭环宿主（R05；迁自旧 appmgr/runtime.c 的最小闭环子集，
 * 合同见 framework/services/package/contract.md §Wasm 宿主）。
 * WAMR 编译面（CMake 消费方接线见 tests/wasm/CMakeLists.txt）：
 * fast 解释器 + INSTRUCTION_METERING（步预算）+ WASI 关 + 池模式
 * （旧 DEC-17/25/31/48③）。
 * 最小闭环：load（已验 slot）→ call（导出函数，步预算到期=E_TIMEOUT
 * 有界停止）→ stop（实例销毁，模块保留——换包重启路径）。
 * natives：本子闭环注册 1 个代表 native（host_read_clock——宿主注入
 * principal 的裁决证明）；全符号集随 R06+ 消费面扩展。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/appstore.h>
#include <entelea/wasm_host.h>
#include <wasm_export.h>

#define MAX_INST 1 /* V1 单实例（旧 DEC-31 语义面） */

struct inst {
	bool used;
	uint32_t app_id;
	wasm_module_inst_t inst;
	wasm_exec_env_t env;
	entelea_principal_t owner;
};
static struct inst insts[MAX_INST];
static bool wamr_inited;
static uint8_t wamr_heap[ENTELEA_WASM_HEAP_SIZE] __aligned(8);

/* 代表 native：宿主注入 principal 裁决（gpio:read:0 门）——证明
 * natives 只信注入身份。Wasm 侧传入整数被忽略。 */
static int32_t native_host_probe(wasm_exec_env_t env,
				 uint32_t ctx_opaque_ignored)
{
	struct inst *it = (struct inst *)wasm_runtime_get_user_data(env);

	(void)ctx_opaque_ignored;
	if (it == NULL) {
		return ENTELEA_E_PERM;
	}
	/* 注入 principal 的裁决（非 Wasm 传参）。 */
	if (entelea_perm_check(it->owner, ENTELEA_PERM_GPIO, ENTELEA_PERM_READ,
			       0) != ENTELEA_OK) {
		return ENTELEA_E_PERM;
	}
	return 1; /* 授权可见 */
}

static NativeSymbol native_syms[] = {
	{
		"host_probe",
		(void *)native_host_probe,
		"(i)i",
		NULL,
	},
};

entelea_result_t entelea_wasm_init(void)
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
	if (wasm_runtime_register_natives("entelea", native_syms,
					  sizeof(native_syms) /
						  sizeof(native_syms[0])) < 0) {
		return ENTELEA_E_IO;
	}
	wamr_inited = true;
	return ENTELEA_OK;
}

static uint8_t *load_bytes(uint8_t slot, const entelea_tsap_view_t *v,
			   uint32_t *len_out)
{
	static uint8_t buf[ENTELEA_APP_SLOT_SIZE / 4]; /* 64KB 装载上限 */

	if (v->wasm_len > sizeof(buf)) {
		return NULL;
	}
	if (entelea_pkg_slot_read(slot, v->wasm_off, buf, v->wasm_len) !=
	    ENTELEA_OK) {
		return NULL;
	}
	*len_out = v->wasm_len;
	return buf;
}

static struct inst *find_free(void)
{
	for (int i = 0; i < MAX_INST; i++) {
		if (!insts[i].used) {
			return &insts[i];
		}
	}
	return NULL;
}

static struct inst *find_app(uint32_t app_id)
{
	for (int i = 0; i < MAX_INST; i++) {
		if (insts[i].used && insts[i].app_id == app_id) {
			return &insts[i];
		}
	}
	return NULL;
}

/* 模块缓存（单模块——V1 单实例；换包 = destroy+重 load）。 */
static wasm_module_t module;
static uint8_t module_slot = 0xFF;

entelea_result_t entelea_wasm_load(uint8_t slot, uint32_t *app_id_out)
{
	if (app_id_out == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	if (!wamr_inited && entelea_wasm_init() != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	entelea_tsap_view_t v;

	if (entelea_pkg_verify(slot, NULL, &v, NULL) != ENTELEA_OK) {
		return ENTELEA_E_INVALID_SIG;
	}
	uint32_t len = 0;
	uint8_t *bytes = load_bytes(slot, &v, &len);

	if (bytes == NULL) {
		return ENTELEA_E_RANGE;
	}
	char err[128];

	if (module != NULL && module_slot != slot) {
		wasm_runtime_unload(module);
		module = NULL;
	}
	if (module == NULL) {
		module = wasm_runtime_load(bytes, len, err, sizeof(err));
		if (module == NULL) {
			return ENTELEA_E_IO;
		}
		module_slot = slot;
	}
	struct inst *it = find_free();

	if (it == NULL) {
		return ENTELEA_E_BUSY; /* 单实例占用（V1） */
	}
	it->inst = wasm_runtime_instantiate(module, ENTELEA_WASM_STACK_SIZE,
					    8192, err, sizeof(err));
	if (it->inst == NULL) {
		return ENTELEA_E_IO;
	}
	it->env = wasm_runtime_create_exec_env(it->inst, ENTELEA_WASM_STACK_SIZE);
	if (it->env == NULL) {
		wasm_runtime_deinstantiate(it->inst);
		it->inst = NULL;
		return ENTELEA_E_IO;
	}
	it->used = true;
	it->app_id = (uint32_t)(it - insts) + 1;
	it->owner = (entelea_principal_t){0};
	wasm_runtime_set_user_data(it->env, it);
	if (app_id_out != NULL) {
		*app_id_out = it->app_id;
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_wasm_call(uint32_t app_id, entelea_principal_t ctx,
				   const char *fn, uint32_t timeout_ms,
				   int32_t *ret_out)
{
	struct inst *it = find_app(app_id);

	if (it == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	if (fn == NULL) {
		return ENTELEA_E_PARAM;
	}
	wasm_function_inst_t f =
		wasm_runtime_lookup_function(it->inst, fn);

	if (f == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	it->owner = ctx; /* natives 裁决身份（注入） */
	uint32_t argv[1] = {0};
	uint32_t argc = 1;

	/* 有界执行：timeout_ms 经步预算指令配额近似（指令/毫秒比率上限
	 * ——DEC-48③ 语义；到期 = WAMR 配额异常 = E_TIMEOUT 有界停止）。 */
	wasm_runtime_set_instruction_count_limit(
		it->env, (uint64_t)timeout_ms * 1000U);
	bool ok = wasm_runtime_call_wasm(it->env, f, argc, argv);

	if (!ok) {
		/* 异常面：配额到期/trap——实例不可信，停止销毁（有界）。 */
		wasm_runtime_clear_exception(it->inst);
		entelea_wasm_stop(app_id);
		return ENTELEA_E_TIMEOUT;
	}
	if (ret_out != NULL) {
		*ret_out = (int32_t)argv[0];
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_wasm_stop(uint32_t app_id)
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

uint8_t entelea_wasm_instance_count(void)
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
void entelea_wasm_test_reset(void)
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
	module_slot = 0xFF;
}
#endif
