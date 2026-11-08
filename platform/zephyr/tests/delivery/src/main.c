/* SPDX-License-Identifier: Apache-2.0 */
/* R12 试用/晋升测试：单候选/内容绑定确认/fail-closed 超时/intake 域外拒。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/delivery.h>

static void *trial_setup(void)
{
	entelea_trial_test_reset();
	return NULL;
}

ZTEST(framework_trial, test_lifecycle_and_user_gate)
{
	entelea_trial_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_trial_candidate_t c;

	memset(&c, 0, sizeof(c));
	c.digest[0] = 0xAB;
	c.permissions = 0x0005;
	c.version = 3;

	/* 未提交即确认/拒绝/结束=STATE。 */
	zassert_equal(entelea_trial_confirm(), ENTELEA_E_STATE);
	zassert_equal(entelea_trial_reject(), ENTELEA_E_STATE);
	zassert_equal(entelea_trial_end(), ENTELEA_E_STATE);
	zassert_equal(entelea_trial_submit(NULL), ENTELEA_E_PARAM);

	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
	/* review P1 负测：PENDING 期不可静默替换确认对象（内容绑定）。 */
	entelea_trial_candidate_t swapped = c;

	swapped.digest[0] = 0xFF;
	zassert_equal(entelea_trial_submit(&swapped), ENTELEA_E_BUSY,
		      "PENDING 期新候选拒");
	entelea_trial_candidate_t cur;

	entelea_trial_current(&cur);
	zassert_equal(cur.digest[0], 0xAB, "确认对象未被替换");

	entelea_trial_current(&cur);
	zassert_equal(cur.digest[0], 0xAB, "内容绑定（digest=确认对象）");
	zassert_equal(cur.permissions, 0x0005);
	zassert_equal(cur.version, 3, "version 同为确认对象（合同三要素）");

	/* 确认前不生效：confirm 是唯一晋升路径。 */
	zassert_equal(entelea_trial_end(), ENTELEA_E_STATE, "PENDING 不可结束");
	zassert_equal(entelea_trial_confirm(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_ACTIVE);
	zassert_equal(entelea_trial_confirm(), ENTELEA_E_STATE, "重复确认拒");

	/* 单候选：ACTIVE 期新候选 BUSY。 */
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_BUSY);
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);
	entelea_trial_current(&cur);
	zassert_equal(cur.digest[0], 0, "结束清槽");
}

ZTEST(framework_trial, test_reject_and_timeout_paths)
{
	entelea_trial_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_trial_candidate_t c;

	memset(&c, 0, sizeof(c));
	c.digest[31] = 1;

	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	/* 拒绝：槽保留可重推。 */
	zassert_equal(entelea_trial_reject(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_REJECTED);
	entelea_trial_candidate_t cur;

	entelea_trial_current(&cur);
	zassert_equal(cur.digest[31], 1, "REJECTED 槽保留（可重推）");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK, "重推=新 PENDING");
	/* 超时：fail-closed。 */
	zassert_equal(entelea_trial_timeout(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_TIMEOUT);
	zassert_equal(entelea_trial_confirm(), ENTELEA_E_STATE,
		      "TIMEOUT 后不可再确认");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK, "TIMEOUT 后可重推");
}

ZTEST(framework_trial, test_factory_intake_gates)
{
	entelea_trial_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_factory_candidate_t fc;

	memset(&fc, 0, sizeof(fc));
	fc.payload_kind = 0; /* wasm */
	fc.budget_steps = 100000;
	fc.mem_quota_kb = 64;

	zassert_equal(entelea_factory_intake(NULL), ENTELEA_E_PARAM);
	zassert_equal(entelea_factory_intake(&fc), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM,
		      "intake→PENDING（确认前不生效）");

	/* 未知 payload_kind 拒（不默认解释）。 */
	entelea_trial_test_reset();
	entelea_factory_candidate_t bad = fc;

	bad.payload_kind = 7;
	zassert_equal(entelea_factory_intake(&bad), ENTELEA_E_PARAM,
		      "未知 payload 拒");
	/* 预算域外。 */
	entelea_trial_test_reset();
	bad = fc;
	bad.budget_steps = 0;
	zassert_equal(entelea_factory_intake(&bad), ENTELEA_E_RANGE);
	bad = fc;
	bad.budget_steps = 10000001U;
	zassert_equal(entelea_factory_intake(&bad), ENTELEA_E_RANGE);
	bad = fc;
	bad.mem_quota_kb = 0;
	zassert_equal(entelea_factory_intake(&bad), ENTELEA_E_RANGE);
	bad = fc;
	bad.mem_quota_kb = 513;
	zassert_equal(entelea_factory_intake(&bad), ENTELEA_E_RANGE);
	/* Lua payload（kind=1）合法路径。 */
	entelea_trial_test_reset();
	bad = fc;
	bad.payload_kind = 1;
	zassert_equal(entelea_factory_intake(&bad), ENTELEA_OK,
		      "Lua payload 合法（不默认解释为 wasm）");
}

ZTEST_SUITE(framework_trial, NULL, trial_setup, NULL, NULL, NULL);
