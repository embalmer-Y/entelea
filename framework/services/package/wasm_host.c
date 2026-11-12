/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Wasm 执行闭环宿主（R05；迁自旧 appmgr/runtime.c 的最小闭环子集，
 * 合同见 framework/services/package/contract.md §Wasm 宿主）。
 * WAMR 编译面（CMake 消费方接线见 tests/wasm/CMakeLists.txt）：
 * fast 解释器 + INSTRUCTION_METERING（步预算）+ WASI 关 + 池模式
 * （旧 DEC-17/25/31/48③）。
 * 最小闭环：load（已验 slot + R12 试用门；owner 此时绑定）→ call（仅
 * owner；试用门复核；导出 ABI 白名单；步预算到期=E_TIMEOUT 有界停止）
 * → stop（实例销毁，模块保留——换包重启路径）。
 * natives：本子闭环注册 1 个代表 native（"entelea"."host_probe" (i)i
 * ——按绑定 owner 裁决）；全符号集随 R06+ 消费面扩展。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <entelea/appstore.h>
#include <entelea/d12.h>
#include <entelea/delivery.h>
#include <entelea/wasm_host.h>
#include <wasm_export.h>

#define MAX_INST 1 /* V1 单实例（旧 DEC-31 语义面） */

struct inst {
	bool used;
	uint32_t app_id;
	wasm_module_inst_t inst;
	wasm_exec_env_t env;
	entelea_principal_t owner;
	uint8_t slot;       /* 装载来源（试用门复核对象） */
	uint8_t digest[32]; /* 装载时整文件 sha256 */
	bool grant_ok;               /* 本次调用的确认授权有效（ACTIVE 且字节相符） */
	entelea_perm_table_t grant;  /* 确认的权限列表（仅 grant_ok 时有效） */
	uint32_t budget_steps;       /* 执行步数上限：确认值，否则设备上限 */
};
static struct inst insts[MAX_INST];
static bool wamr_inited;
static uint8_t wamr_heap[ENTELEA_WASM_HEAP_SIZE] __aligned(8);

/* 代表 native：宿主注入 principal 裁决（gpio:read:0 门）——证明
 * natives 只信注入身份。Wasm 侧传入整数被忽略。 */
/* D-B 取交集：确认的权限列表与宿主注入的 principal 授权都须允许。 */
static bool app_allows(const struct inst *it, entelea_perm_class_t cls,
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

static int32_t probe_decide(const struct inst *it, uint32_t ctx_opaque_ignored)
{
	(void)ctx_opaque_ignored;
	if (!app_allows(it, ENTELEA_PERM_GPIO, ENTELEA_PERM_READ, 0)) {
		return ENTELEA_E_PERM;
	}
	return 1; /* 授权可见 */
}

static int32_t native_host_probe(wasm_exec_env_t env,
				 uint32_t ctx_opaque_ignored)
{
	struct inst *it = (struct inst *)wasm_runtime_get_user_data(env);

	return probe_decide(it, ctx_opaque_ignored);
}

#ifdef ENTELEA_TEST
/* 裁决函数直达钩子（伪造/失效 principal、确认权限缺失等难以经 load 构造的
 * 输入）；grant=NULL 表示没有确认权限。VM→native 真路径由 tests/wasm 覆盖。 */
int32_t entelea_wasm_test_probe_decide(entelea_principal_t owner,
				       const char *grant)
{
	struct inst fake;

	memset(&fake, 0, sizeof(fake));
	fake.owner = owner;
	if (grant != NULL) {
		entelea_perm_table_init(&fake.grant);
		if (entelea_perm_parse_list(grant, strlen(grant), &fake.grant) !=
		    ENTELEA_OK) {
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
	/* RF1：返回 bool（原 `< 0` 恒假——失败被吞）。无 natives 的宿主
	 * 不得报 OK：回滚运行时，未初始化态可重试。 */
	if (!wasm_runtime_register_natives("entelea", native_syms,
					   sizeof(native_syms) /
						   sizeof(native_syms[0]))) {
		wasm_runtime_destroy();
		return ENTELEA_E_IO;
	}
	wamr_inited = true;
	return ENTELEA_OK;
}

/* 装载缓冲：WAMR 要求可写且存活至 unload——仅在无模块引用时覆写。
 * 单趟捕获（store.c entelea_pkg_file_digest_capture）写入此处。 */
static uint8_t load_buf[ENTELEA_APP_SLOT_SIZE / 4]; /* 64KB 装载上限 */

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

/* 唯一模块（V1 单实例）：活实例引用它与装载缓冲，存活期间不得卸载/覆写。 */
static wasm_module_t module;

static bool same_principal(entelea_principal_t a, entelea_principal_t b)
{
	return a._handle == b._handle && a._gen == b._gen &&
	       a.app_id == b.app_id;
}

/* 导出 ABI 白名单 ()->i32 | (i32)->i32：返回形参个数；不符 = -1。 */
static int abi_params(wasm_function_inst_t f, wasm_module_inst_t mi)
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

entelea_result_t entelea_wasm_load(uint8_t slot, entelea_principal_t owner,
				   uint32_t *app_id_out)
{
	if (app_id_out == NULL || slot >= ENTELEA_APP_SLOT_COUNT) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t rc = entelea_wasm_init();

	if (rc != ENTELEA_OK) {
		return rc;
	}
	/* RF1 P1-7：占用先拒且零副作用（原先卸模块后查空槽——活实例引用
	 * 已释放模块）。 */
	struct inst *it = find_free();

	if (it == NULL || entelea_wasm_instance_count() != 0) {
		return ENTELEA_E_BUSY;
	}
	entelea_tsap_view_t v;

	rc = entelea_pkg_verify(slot, NULL, &v, NULL);
	if (rc != ENTELEA_OK) {
		return rc; /* RF1：原样传播（介质 E_IO ≠ 包坏 E_INVALID_SIG） */
	}
	if (v.wasm_len > sizeof(load_buf)) {
		return ENTELEA_E_RANGE; /* 64KB 装载上限（无副作用） */
	}
	/* RF1 #9：R12 试用门——候选 slot 仅用户已确认的那份字节可装载
	 * （摘要域=Forge package_digest 域）。拒绝先于卸旧模块（零副作用）。 */
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
	/* 每次 load 卸旧重建：同槽重暂存后旧缓存不得复活（验签对象与执行
	 * 对象同为新字节）。先卸再覆写装载缓冲。 */
	if (module != NULL) {
		wasm_runtime_unload(module);
		module = NULL;
	}
	/* F3：装载字节与门摘要来自同一趟读取；不相等 = 拒绝装载（模块已卸、
	 * 未创建实例）。 */
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
	it->env = wasm_runtime_create_exec_env(it->inst, ENTELEA_WASM_STACK_SIZE);
	if (it->env == NULL) {
		wasm_runtime_deinstantiate(it->inst);
		it->inst = NULL;
		return ENTELEA_E_IO;
	}
	it->used = true;
	it->app_id = (uint32_t)(it - insts) + 1;
	it->owner = owner; /* RF1 P1-8：身份在 load 绑定，call 不可替换 */
	it->slot = slot;
	memcpy(it->digest, dg, sizeof(it->digest));
	it->grant_ok = false;
	entelea_perm_table_init(&it->grant);
	it->budget_steps = ENTELEA_WASM_DEFAULT_INSTRUCTIONS;
	wasm_runtime_set_user_data(it->env, it);
	*app_id_out = it->app_id;
	return ENTELEA_OK;
}

entelea_result_t entelea_wasm_call(uint32_t app_id, entelea_principal_t ctx,
				   const char *fn, int32_t arg,
				   uint32_t timeout_ms, int32_t *ret_out)
{
	struct inst *it = find_app(app_id);

	if (it == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	if (fn == NULL) {
		return ENTELEA_E_PARAM;
	}
	/* RF1 P1-8：仅绑定 owner 可调用（拒绝不执行、不销毁实例）。 */
	if (!same_principal(ctx, it->owner)) {
		return ENTELEA_E_PERM;
	}
	{ /* RF1 #9：试用门复核（装载后同槽新候选/拒绝/超时 → E_PERM；
	   * 不执行、不销毁实例） */
		entelea_result_t tg = entelea_trial_gate(it->slot, it->digest);

		if (tg != ENTELEA_OK) {
			return tg;
		}
	}
	{ /* R12 授权查询（逐次，不缓存；D-B/D-C）：仅 ACTIVE 且字节相符时授予
	   * 确认的权限与步数。其余（NONE/非候选/试用结束）无 natives 权限（A1），
	   * 执行配额取设备上限。 */
		entelea_trial_grant_t g;
		entelea_result_t gr = entelea_trial_grant(it->slot, it->digest, &g);

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
	{ /* D12 冷却窗前置（异常复位后 60s——E_BUSY 拒执行） */
		entelea_result_t gate = entelea_d12_wasm_allow();

		if (gate != ENTELEA_OK) {
			return gate;
		}
	}
	wasm_function_inst_t f =
		wasm_runtime_lookup_function(it->inst, fn);

	if (f == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	/* RF1 P1-8：签名不在白名单 / 无参函数收到非零 arg = E_PARAM（不执行、
	 * 不销毁；原硬编码 argv={0} 静默丢弃入参）。 */
	int np = abi_params(f, it->inst);

	if (np < 0 || (np == 0 && arg != 0)) {
		return ENTELEA_E_PARAM;
	}
	uint32_t argv[1] = {(uint32_t)arg};

	/* 有界执行：timeout_ms 经步预算指令配额近似（1000 指令/ms——DEC-48③
	 * 语义；到期 = WAMR 配额异常 = E_TIMEOUT 有界停止）。WAMR 形参为
	 * int，封顶 1e8（D12 owner v1 配额）远低于 INT_MAX，不会截断为负。
	 * timeout_ms=0 = 零配额（首条指令即到期），不是“默认/无界”。 */
	uint32_t quota = timeout_ms;

	if (quota > ENTELEA_WASM_DEFAULT_INSTRUCTIONS /
			    ENTELEA_WASM_INSTRUCTIONS_PER_MS) {
		quota = ENTELEA_WASM_DEFAULT_INSTRUCTIONS /
			ENTELEA_WASM_INSTRUCTIONS_PER_MS;
	}
	uint32_t limit = quota * ENTELEA_WASM_INSTRUCTIONS_PER_MS;

	if (limit > it->budget_steps) {
		limit = it->budget_steps; /* D-C：min(请求, 确认步数, 设备上限) */
	}
	wasm_runtime_set_instruction_count_limit(it->env, (int)limit);
	bool ok = wasm_runtime_call_wasm(it->env, f, (uint32_t)np, argv);

	if (!ok) {
		/* 异常面（E-2 收窄）：配额到期 ≠ trap——到期=E_TIMEOUT
		 * （有界停止语义，DEC-48③）；其余 trap=E_IO。两者实例都
		 * 不可信，停止销毁。判别=WAMR 异常串（fast/classic 解释器
		 * 同源 "instruction limit exceeded"——pinned 25bd7eb）。 */
		const char *ex = wasm_runtime_get_exception(it->inst);
		bool quota = ex != NULL && strstr(ex, "instruction limit") != NULL;

		wasm_runtime_clear_exception(it->inst);
		entelea_wasm_stop(app_id);
		return quota ? ENTELEA_E_TIMEOUT : ENTELEA_E_IO;
	}
	if (ret_out != NULL) {
		*ret_out = (int32_t)argv[0];
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_wasm_call_default(uint32_t app_id,
					  entelea_principal_t ctx,
					  const char *fn, int32_t arg,
					  int32_t *ret_out)
{
	return entelea_wasm_call(app_id, ctx, fn, arg,
				 ENTELEA_WASM_DEFAULT_INSTRUCTIONS /
					 ENTELEA_WASM_INSTRUCTIONS_PER_MS,
				 ret_out);
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
	if (wamr_inited) { /* 运行时与 natives 一并回收：每用例从零 init */
		wasm_runtime_destroy();
		wamr_inited = false;
	}
}
#endif
