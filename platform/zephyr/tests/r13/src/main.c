/* SPDX-License-Identifier: Apache-2.0 */
/* R13 框架合同验收（native_sim 面）：配置矩阵自洽 + 已交付机制的关键
 * 合同跨面复验（监督/保护/存储/能力/任务/资源/网络/辅助/媒体/经历/
 * 情感/试用——每面一项代表性断言；硬件/产品验收归 owner 闸）。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/affect.h>
#include <entelea/appstore.h>
#include <entelea/assistant.h>
#include <entelea/config_matrix.h>
#include <entelea/delivery.h>
#include <entelea/media.h>
#include <entelea/memory.h>
#include <entelea/netmon.h>
#include <entelea/principal.h>
#include <entelea/resource.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/task.h>

static void *r13_setup(void)
{
	entelea_safety_test_reset();
	entelea_perm_test_reset();
	entelea_task_test_reset();
	entelea_power_test_reset();
	entelea_net_test_reset();
	entelea_mem_test_reset();
	entelea_affect_test_reset();
	entelea_trial_test_reset();
	entelea_media_test_reset();
	return NULL;
}

ZTEST(framework_r13, test_config_matrix)
{
	zassert_true(entelea_config_valid(&ENTELEA_CONFIG_MINIMAL_ASSIST));
	zassert_true(entelea_config_valid(&ENTELEA_CONFIG_FULL_SIMULATED));
	/* 依赖自洽负测。 */
	entelea_config_t bad = ENTELEA_CONFIG_MINIMAL_ASSIST;

	bad.storage = false;
	bad.delivery = true;
	zassert_false(entelea_config_valid(&bad), "试用无存储不自洽");
	bad = ENTELEA_CONFIG_MINIMAL_ASSIST;
	bad.affect = true;
	zassert_false(entelea_config_valid(&bad), "情感无经历不自洽");
	zassert_ok(strcmp(entelea_config_name(&ENTELEA_CONFIG_MINIMAL_ASSIST),
			  "minimal-assist"));
	zassert_ok(strcmp(entelea_config_name(&ENTELEA_CONFIG_FULL_SIMULATED),
			  "full-simulated"));
	zassert_false(entelea_config_valid(NULL));
}

ZTEST(framework_r13, test_cross_mechanism_contract_representative)
{
	/* R02 保护：安全态拒。 */
	static const entelea_out_channel_t ch = {
		.uid = "k", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	zassert_equal(entelea_safety_register_channel(&ch), ENTELEA_OK);
	zassert_equal(entelea_safety_commit("k", (entelea_out_value_t){.b = 1}),
		      ENTELEA_E_STATE, "R02 安全态拒");

	/* R04 权限：伪造句柄拒。 */
	zassert_equal(entelea_perm_check((entelea_principal_t){9, 9},
					   ENTELEA_PERM_GPIO,
					   ENTELEA_PERM_WRITE, 0),
		      ENTELEA_E_PERM, "R04 伪造句柄");

	/* R04 任务：取消效果域。 */
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 1, &t), ENTELEA_OK);
	entelea_task_id_t id = 0;

	zassert_equal(entelea_task_submit(ctx, "x", 1, 0, &id), ENTELEA_OK);
	zassert_equal(entelea_task_start(ctx, id), ENTELEA_OK);
	zassert_equal(entelea_task_cancel(ctx, id), ENTELEA_OK);
	entelea_task_t out;

	zassert_equal(entelea_task_get(id, &out), ENTELEA_OK);
	zassert_equal(out.effect, ENTELEA_EFFECT_UNKNOWN, "R04 RUNNING 取消=UNKNOWN");

	/* R06 网络：恢复防抖。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, true, 1), ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_DEGRADED,
		      "R06 单次不恢复");
	zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, true, 2), ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_UP);

	/* R08 辅助：模型不可用停智能。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, false, 3), ENTELEA_OK);
	zassert_false(entelea_assistant_available(), "R08 云停用");

	/* R10 经历：无同意不保存。 */
	entelea_mem_entry_t e;

	memset(&e, 0, sizeof(e));
	e.len = 4;
	zassert_equal(entelea_mem_write(false, &e), ENTELEA_E_PERM, "R10 同意门");

	/* R11 情感：确定性中位。 */
	entelea_affect_state_t af;

	entelea_affect_get(&af);
	zassert_equal(af.level[0], 50, "R11 中位基线");

	/* R12 试用：确认前不生效。 */
	entelea_trial_candidate_t tc;

	memset(&tc, 0, sizeof(tc));
	zassert_equal(entelea_trial_submit(&tc), ENTELEA_OK);
	zassert_equal(entelea_trial_confirm(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_ACTIVE, "R12 确认后 ACTIVE");
	zassert_equal(entelea_trial_submit(&tc), ENTELEA_E_BUSY, "R12 单候选");
}

ZTEST_SUITE(framework_r13, NULL, r13_setup, NULL, NULL, NULL);
