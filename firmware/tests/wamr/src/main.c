/* SPDX-License-Identifier: Apache-2.0 */
/* framework.wamr（M2b.2a 环境批）：WAMR 装载/实例化/调用冒烟——native_sim 上
 * 验证 DEC-25 执行模式配置（fast 解释器 + WASI 关）真实可跑。
 * 判据：① 零导入面（无导入注册即实例化成功 = WASI 关 + ts_* 导入面为空的
 * 边界证明）；② health_ping/on_input 调用返回确定值（合同 9 友好：纯函数）。
 * 注：Q-23（宿主线程模型）裁定前的构建级/装载级验证——不经 appmgr 接线，
 * 测试线程直调 WAMR API，不引入生产线程语义。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <wasm_export.h>
#include "wasm_bytes.h"

ZTEST(framework_wamr, test_01_load_instantiate_call)
{
	char err[128];
	zassert_true(wasm_runtime_init(), "runtime init");
	wasm_module_t mod = wasm_runtime_load(wasm_bytes, sizeof(wasm_bytes), err, sizeof(err));
	zassert_not_null(mod, "load: %s", err);

	/* 零导入面证明：不注册任何 native 符号直接实例化 */
	wasm_module_inst_t inst = wasm_runtime_instantiate(mod, 4096, 8192, err, sizeof(err));
	zassert_not_null(inst, "instantiate（零导入）: %s", err);

	wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 4096);
	zassert_not_null(env, "exec env");

	/* health_ping（manifest V1 必选导出）→ 恒 1 */
	wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "health_ping");
	zassert_not_null(fn, "lookup health_ping");
	uint32_t argv[1] = {0};
	zassert_true(wasm_runtime_call_wasm(env, fn, 0, argv), "call health_ping");
	zassert_equal(argv[0], 1, "health_ping 返回");

	/* on_input（确定性纯函数）：21 → 43 */
	fn = wasm_runtime_lookup_function(inst, "on_input");
	zassert_not_null(fn, "lookup on_input");
	argv[0] = 21;
	zassert_true(wasm_runtime_call_wasm(env, fn, 1, argv), "call on_input");
	zassert_equal(argv[0], 43, "on_input(21) = 43（确定性）");
	/* 同输入重放（合同 9 方向性自检） */
	uint32_t again[1] = {21};
	zassert_true(wasm_runtime_call_wasm(env, fn, 1, again), "call on_input again");
	zassert_equal(again[0], 43, "重放一致");

	wasm_runtime_destroy_exec_env(env);
	wasm_runtime_deinstantiate(inst);
	wasm_runtime_unload(mod);
	wasm_runtime_destroy();
}

ZTEST_SUITE(framework_wamr, NULL, NULL, NULL, NULL, NULL);
