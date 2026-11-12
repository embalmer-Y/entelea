/* SPDX-License-Identifier: Apache-2.0 */
/* R12 试用/晋升测试：单候选/内容绑定确认/fail-closed 超时/intake 域外拒。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/appstore.h>
#include <entelea/delivery.h>
#include <entelea/kv.h>
#include <entelea/storage.h>

static void trial_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_kv_test_reset();
	entelea_trial_test_reset();
}

/* 状态机用例直接提交调用方摘要（TEST 接缝，不验签）。生产 submit 由设备
 * 计算摘要，其验证在 wasm 套件用签名包完成（F4）。 */
#define entelea_trial_submit entelea_trial_submit_with_digest_for_test

/* 候选必须通过 admission：步数 1..1e8，权限列表合法（空 = 无权限）。 */
#define TEST_BUDGET 1000U

static void set_perm(entelea_trial_candidate_t *c, const char *perm)
{
	size_t n = strlen(perm);

	zassert_true(n <= ENTELEA_TRIAL_PERM_MAX);
	memcpy(c->perm, perm, n);
	c->perm_len = (uint8_t)n;
}

ZTEST(framework_trial, test_lifecycle_and_user_gate)
{
	entelea_trial_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_trial_candidate_t c;

	memset(&c, 0, sizeof(c));
	c.digest[0] = 0xAB;
	set_perm(&c, "gpio:read:0;pwm:set:1");
	c.budget_steps = TEST_BUDGET;
	c.version = 3;

	/* 未提交即确认/拒绝/结束=STATE。 */
	zassert_equal(entelea_trial_confirm(&c), ENTELEA_E_STATE);
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
	zassert_equal(memcmp(cur.perm, "gpio:read:0;pwm:set:1", 21), 0,
		      "权限列表为确认对象");
	zassert_equal(cur.budget_steps, TEST_BUDGET, "步数为确认对象");
	zassert_equal(cur.version, 3, "version 同为确认对象（合同三要素）");

	/* 确认前不生效：confirm（携带所见对象）是唯一晋升路径。 */
	zassert_equal(entelea_trial_end(), ENTELEA_E_STATE, "PENDING 不可结束");
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_ACTIVE);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_E_STATE, "重复确认拒");

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
	c.budget_steps = TEST_BUDGET;

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
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_E_STATE,
		      "TIMEOUT 后不可再确认");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK, "TIMEOUT 后可重推");
}

ZTEST(framework_trial, test_factory_intake_domain_refusals)
{
	entelea_trial_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_factory_candidate_t fc;

	memset(&fc, 0, sizeof(fc));
	fc.payload_kind = 0; /* wasm */
	fc.budget_steps = 100000;
	fc.mem_quota_kb = 64;
	fc.permissions_len = 11;
	memcpy(fc.permissions, "gpio:read:0", 11);

	/* 域检查先于任何 slot 访问：拒绝不触发状态恢复。 */
	zassert_equal(entelea_factory_intake(NULL, 0), ENTELEA_E_PARAM);
	zassert_equal(entelea_factory_intake(&fc, ENTELEA_APP_SLOT_COUNT),
		      ENTELEA_E_PARAM, "slot 域外");

	/* 未知 payload_kind 拒（不默认解释）。 */
	entelea_factory_candidate_t bad = fc;

	bad.payload_kind = 7;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_E_PARAM,
		      "未知 payload 拒");
	/* 预算、内存与权限长度域外。 */
	bad = fc;
	bad.budget_steps = 0;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_E_RANGE);
	bad = fc;
	bad.budget_steps = 10000001U;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_E_RANGE);
	bad = fc;
	bad.mem_quota_kb = 0;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_E_RANGE);
	bad = fc;
	bad.mem_quota_kb = 513;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_E_RANGE);
	bad = fc;
	bad.permissions_len = ENTELEA_TRIAL_PERM_MAX + 1U;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_E_RANGE,
		      "权限列表超长拒");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_UNINITIALIZED,
		      "域外拒绝不改变状态");
}

/* ---- 跨重启持久（A-4）：flush/restore 往返+fail-closed+约束态保持 ---- */
ZTEST(framework_trial, test_persistence_roundtrip_and_fail_closed)
{
	entelea_trial_test_reset();
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);

	/* 无记录：restore=幂等 NONE。 */
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);

	/* PENDING 往返：模拟重启=模块态清零后 restore。 */
	entelea_trial_candidate_t c;

	memset(&c, 0, sizeof(c));
	c.digest[7] = 0x5A;
	set_perm(&c, "gpio:read:0");
	c.budget_steps = TEST_BUDGET;
	c.version = 9;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_candidate_t before;

	entelea_trial_current(&before);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	entelea_trial_test_reset(); /* 模拟重启丢内存态 */
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_UNINITIALIZED);
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM,
		      "PENDING 跨重启存活");
	entelea_trial_candidate_t cur;

	entelea_trial_current(&cur);
	zassert_equal(cur.digest[7], 0x5A);
	zassert_equal(memcmp(cur.perm, "gpio:read:0", 11), 0, "权限列表跨重启");
	zassert_equal(cur.budget_steps, TEST_BUDGET, "步数跨重启");
	zassert_equal(cur.version, 9);
	zassert_equal(cur.slot, before.slot);
	zassert_equal(cur.epoch, before.epoch, "纪元跨重启（重启前展示仍有效）");
	/* 恢复后的约束语义仍在：confirm 可达（唯一晋升）；PENDING 期
	 * submit=BUSY 保持。 */
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_BUSY,
		      "恢复后约束语义不放宽");
	zassert_equal(entelea_trial_confirm(&before), ENTELEA_OK);

	/* TIMEOUT 约束态跨重启：confirm 拒保持。 */
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_timeout(), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_TIMEOUT,
		      "TIMEOUT 跨重启保持");
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_E_STATE,
		      "跨重启不可绕过 fail-closed 撤销");

	/* 坏记录（域外 state）→ fail-closed 约束态。RF1 #9 合同变更：原
	 * 归 NONE=试用政策不管辖=未确认字节放行（fail-open）；现置 TIMEOUT
	 * 且候选槽 UNKNOWN（两槽门拒）。 */
	uint8_t bad[141];

	zassert_equal(entelea_storage_meta_read(bad, sizeof(bad), NULL),
		      ENTELEA_E_PARAM);
	{
		uint16_t blen = 0;

		zassert_equal(entelea_storage_meta_read(bad, sizeof(bad), &blen),
			      ENTELEA_OK);
		zassert_equal(blen, 141);
		bad[1] = 0x77; /* 域外 state */
		zassert_equal(entelea_storage_meta_write(bad, blen), ENTELEA_OK);
		zassert_equal(entelea_storage_meta_write(bad, blen), ENTELEA_OK);
	}
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_E_IO);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_TIMEOUT,
		      "坏记录 fail-closed 约束态");
	entelea_trial_current(&cur);
	zassert_equal(cur.slot, ENTELEA_TRIAL_SLOT_UNKNOWN, "候选槽不可知");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_STATE,
		      "未知候选不得被普通提交覆盖");
	/* 清槽也持久：end→flush→restore=NONE。 */
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE, "清槽持久");
	entelea_storage_test_reset();
	entelea_trial_test_reset();
}

ZTEST_SUITE(framework_trial, NULL, NULL, trial_before, NULL, NULL);

/* RF1 #9（G4）：迟到确认不得绑定到用户未见过的新候选（确认须携带
 * 所见对象；原 confirm() 无参——确认 A 的操作会激活 B）。 */
ZTEST(framework_trial, test_rf1_stale_confirm_rejected)
{
	entelea_trial_test_reset();
	entelea_trial_candidate_t a;
	entelea_trial_candidate_t b;
	entelea_trial_candidate_t shown_a;
	entelea_trial_candidate_t shown_b;

	memset(&a, 0, sizeof(a));
	a.digest[0] = 0xA1;
	set_perm(&a, "gpio:read:0");
	a.budget_steps = TEST_BUDGET;
	zassert_equal(entelea_trial_submit(&a), ENTELEA_OK);
	entelea_trial_current(&shown_a); /* 界面展示 A */
	/* 确认窗超时；工厂随后推 B（更高权限）。 */
	zassert_equal(entelea_trial_timeout(), ENTELEA_OK);
	b = a;
	b.digest[0] = 0xB2;
	set_perm(&b, "gpio:read:0;pwm:set:1");
	zassert_equal(entelea_trial_submit(&b), ENTELEA_OK);
	zassert_equal(entelea_trial_confirm(&shown_a), ENTELEA_E_PERM,
		      "迟到的“确认 A”");
	zassert_not_equal(entelea_trial_state(), ENTELEA_TRIAL_ACTIVE,
			  "迟到确认激活了用户未见过的候选");
	entelea_trial_current(&shown_b);
	zassert_equal(entelea_trial_confirm(&shown_b), ENTELEA_OK,
		      "确认 B 须展示 B");
}

/* RF1 #9（G4）：确认对象逐字段绑定；输入纪元被忽略（本层赋值）。 */
ZTEST(framework_trial, test_rf1_confirm_binds_every_field)
{
	entelea_trial_test_reset();
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t shown;
	entelea_trial_candidate_t bad;

	memset(&c, 0, sizeof(c));
	c.digest[3] = 0x33;
	set_perm(&c, "gpio:read:0");
	c.budget_steps = TEST_BUDGET;
	c.version = 5;
	c.slot = 1;
	c.epoch = 0xDEADBEEFU;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_current(&shown);
	zassert_equal(shown.slot, 1);
	zassert_equal(shown.epoch, 1U, "首个纪元=1（输入纪元被忽略）");
	zassert_equal(entelea_trial_confirm(NULL), ENTELEA_E_PARAM);
	bad = shown;
	bad.digest[3] ^= 1U;
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "digest");
	bad = shown;
	bad.perm[0] = 'x';
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "perm 列表");
	bad = shown;
	bad.perm_len = 0;
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "perm 长度");
	bad = shown;
	bad.budget_steps++;
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "budget");
	bad = shown;
	bad.version++;
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "version");
	bad = shown;
	bad.slot = 0;
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "slot");
	bad = shown;
	bad.epoch++;
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "epoch");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM,
		      "不符确认保持 PENDING");
	zassert_equal(entelea_trial_confirm(&shown), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_ACTIVE);
}

/* RF1 #9（G4）：同内容重推=新纪元；已拒展示的迟到确认不生效。 */
ZTEST(framework_trial, test_rf1_repush_new_epoch)
{
	entelea_trial_test_reset();
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t first;
	entelea_trial_candidate_t second;

	memset(&c, 0, sizeof(c));
	c.digest[0] = 0x11;
	c.budget_steps = TEST_BUDGET;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_current(&first);
	zassert_equal(entelea_trial_reject(), ENTELEA_OK);
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK, "同槽重推");
	entelea_trial_current(&second);
	zassert_equal(second.epoch, first.epoch + 1U, "重推=新纪元");
	zassert_equal(entelea_trial_confirm(&first), ENTELEA_E_PERM,
		      "已拒展示的迟到确认");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
	zassert_equal(entelea_trial_confirm(&second), ENTELEA_OK);
}

ZTEST(framework_trial, test_epoch_exhaustion_cannot_revive_old_confirmation)
{
	entelea_trial_candidate_t candidate = {.budget_steps = TEST_BUDGET};
	entelea_trial_candidate_t old_shown;
	/* v3 记录 141B：ver|state|version|slot|epoch[4]|budget[4]|perm_len|
	 * perm[96]|digest[32]；digest 偏移 13+96=109。 */
	uint8_t record[141] = {0};

	candidate.digest[0] = 0x11;
	zassert_equal(entelea_trial_submit(&candidate), ENTELEA_OK);
	entelea_trial_current(&old_shown);
	zassert_equal(old_shown.epoch, 1U);
	zassert_equal(entelea_trial_timeout(), ENTELEA_OK);

	record[0] = 3;
	record[1] = ENTELEA_TRIAL_TIMEOUT;
	memset(record + 4, 0xFF, 4); /* 纪元耗尽 */
	record[8] = 0xE8;            /* budget = 1000 (LE) */
	record[9] = 0x03;
	memcpy(record + 109, candidate.digest, sizeof(candidate.digest));
	zassert_equal(entelea_storage_meta_write(record, sizeof(record)), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_write(record, sizeof(record)), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);

	entelea_result_t rc = entelea_trial_submit(&candidate);

	zassert_equal(rc, ENTELEA_E_RANGE, "exhausted epoch must refuse a new candidate");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_TIMEOUT);
	zassert_not_equal(entelea_trial_confirm(&old_shown), ENTELEA_OK,
			  "wrapped epoch must not revive an old confirmation");
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_submit(&candidate), ENTELEA_E_RANGE,
		      "epoch exhaustion remains closed after restart");
}

/* RF1 #9（G4）：slot 域与槽保留——REJECTED/TIMEOUT 期换槽 BUSY（被拒字节
 * 不得因换槽脱离试用门）；NONE 后任意合法槽；intake 绑定 slot。 */
ZTEST(framework_trial, test_rf1_submit_slot_rules)
{
	entelea_trial_test_reset();
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t cur;
	entelea_factory_candidate_t fc;

	memset(&c, 0, sizeof(c));
	c.budget_steps = TEST_BUDGET;
	c.slot = ENTELEA_APP_SLOT_COUNT;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_PARAM, "slot 域外");
	c.slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_PARAM, "UNKNOWN 不可提交");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_UNINITIALIZED);
	c.slot = 0;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_reject(), ENTELEA_OK);
	c.slot = 1;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_BUSY, "REJECTED 换槽");
	c.slot = 0;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK, "同槽重推");
	zassert_equal(entelea_trial_timeout(), ENTELEA_OK);
	c.slot = 1;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_BUSY, "TIMEOUT 换槽");
	c.slot = 0;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	c.slot = 1;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK, "NONE 后可换槽");

	entelea_trial_test_reset();
	memset(&fc, 0, sizeof(fc));
	fc.budget_steps = 1000;
	fc.mem_quota_kb = 8;
	zassert_equal(entelea_factory_intake(&fc, ENTELEA_APP_SLOT_COUNT),
		      ENTELEA_E_PARAM, "intake slot 域外");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_UNINITIALIZED);
}

/* RF1 #9（G4）：执行门——候选槽仅 ACTIVE 且同字节放行；非候选槽与 NONE
 * 不管辖（稳定集准入=D05）。 */
ZTEST(framework_trial, test_rf1_gate_semantics)
{
	entelea_trial_test_reset();
	uint8_t dg[32];
	uint8_t other[32];
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t cur;

	memset(dg, 0x42, sizeof(dg));
	memset(other, 0x43, sizeof(other));
	zassert_equal(entelea_trial_gate(0, NULL), ENTELEA_E_PARAM);
	zassert_equal(entelea_trial_gate(0, dg), ENTELEA_OK, "NONE 不管辖");
	memset(&c, 0, sizeof(c));
	memcpy(c.digest, dg, sizeof(dg));
	c.budget_steps = TEST_BUDGET;
	c.slot = 1;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_gate(1, dg), ENTELEA_E_PERM, "PENDING");
	zassert_equal(entelea_trial_gate(0, other), ENTELEA_OK, "非候选槽");
	zassert_equal(entelea_trial_reject(), ENTELEA_OK);
	zassert_equal(entelea_trial_gate(1, dg), ENTELEA_E_PERM, "REJECTED");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_timeout(), ENTELEA_OK);
	zassert_equal(entelea_trial_gate(1, dg), ENTELEA_E_PERM, "TIMEOUT");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_gate(1, dg), ENTELEA_OK, "ACTIVE 同字节");
	zassert_equal(entelea_trial_gate(1, other), ENTELEA_E_PERM,
		      "确认后换字节");
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	zassert_equal(entelea_trial_gate(1, other), ENTELEA_OK,
		      "结束后试用政策不管辖（残余：D05）");
}

static void restore_from(const uint8_t *rec, uint16_t len)
{
	zassert_equal(entelea_storage_meta_write(rec, len), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_write(rec, len), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_E_IO);
}

static void assert_unknown_closed(const char *what)
{
	entelea_trial_candidate_t cur;
	uint8_t any[32];

	memset(any, 0, sizeof(any));
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_TIMEOUT, "%s", what);
	zassert_equal(cur.slot, ENTELEA_TRIAL_SLOT_UNKNOWN, "%s", what);
	zassert_equal(entelea_trial_gate(0, any), ENTELEA_E_PERM, "%s", what);
	zassert_equal(entelea_trial_gate(1, any), ENTELEA_E_PERM, "%s", what);
}

static entelea_store_ops_t meta_read_base;
static unsigned int meta_erases;

static entelea_result_t fail_meta_read(entelea_store_part_t part, uint32_t off,
				       void *buf, uint32_t len)
{
	if (part == ENTELEA_PART_META) {
		return ENTELEA_E_IO;
	}
	return meta_read_base.read(part, off, buf, len);
}

static entelea_result_t fail_second_meta_copy(entelea_store_part_t part,
					     uint32_t off, void *buf,
					     uint32_t len)
{
	if (part == ENTELEA_PART_META && off >= ENTELEA_STORAGE_META_REC) {
		return ENTELEA_E_IO;
	}
	return meta_read_base.read(part, off, buf, len);
}

static entelea_result_t fail_second_meta_erase(entelea_store_part_t part,
					      uint32_t off, uint32_t len)
{
	if (part == ENTELEA_PART_META && ++meta_erases == 2U) {
		return ENTELEA_E_IO;
	}
	return meta_read_base.erase_off(part, off, len);
}

ZTEST(framework_trial, test_restart_gate_restores_pending_before_load)
{
	entelea_trial_candidate_t candidate = {.budget_steps = TEST_BUDGET};

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trial_test_reset();
	candidate.digest[0] = 0xA7;
	candidate.slot = 0;
	zassert_equal(entelea_trial_submit(&candidate), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);

	entelea_trial_test_reset();
	zassert_equal(entelea_trial_gate(0, candidate.digest), ENTELEA_E_PERM,
		      "pending record must be restored before the execution gate");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
}

ZTEST(framework_trial, test_restore_cannot_discard_live_unflushed_candidate)
{
	entelea_trial_candidate_t candidate = {.budget_steps = TEST_BUDGET};

	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_submit(&candidate), ENTELEA_OK);
	zassert_equal(entelea_trial_restore(), ENTELEA_OK, "idempotent after init");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
	zassert_equal(entelea_trial_gate(0, candidate.digest), ENTELEA_E_PERM);
}

ZTEST(framework_trial, test_corrupt_meta_cannot_open_gate)
{
	entelea_trial_candidate_t candidate = {.budget_steps = TEST_BUDGET};

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trial_test_reset();
	candidate.digest[0] = 0xA7;
	zassert_equal(entelea_trial_submit(&candidate), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_corrupt_test(0), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_corrupt_test(1), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_E_IO,
		      "corrupt copies are not a fresh empty record");
	assert_unknown_closed("double corruption");
}

ZTEST(framework_trial, test_partial_meta_is_not_an_erased_store)
{
	uint8_t garbage = 0;
	uint8_t dg[32] = {0};

	zassert_equal(entelea_storage_test_backend_write(ENTELEA_PART_META,
							900, &garbage, 1),
		      ENTELEA_OK);
	zassert_equal(entelea_trial_gate(0, dg), ENTELEA_E_IO);
	assert_unknown_closed("non-erased byte outside record headers");
}

ZTEST(framework_trial, test_meta_read_failure_cannot_open_gate)
{
	entelea_store_ops_t failure;
	entelea_result_t result;

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trial_test_reset();
	meta_read_base = entelea_store_backend;
	failure = meta_read_base;
	failure.read = fail_meta_read;
	zassert_equal(entelea_storage_mount(&failure), ENTELEA_OK);
	result = entelea_trial_restore();
	zassert_equal(entelea_storage_mount(&meta_read_base), ENTELEA_OK);
	zassert_equal(result, ENTELEA_E_IO, "backend failure is not an empty record");
	assert_unknown_closed("unreadable storage");
}

ZTEST(framework_trial, test_no_fallback_to_stale_copy_on_io_error)
{
	uint8_t dg[32] = {0};
	entelea_trial_candidate_t c = {.slot = 0, .budget_steps = TEST_BUDGET};
	entelea_store_ops_t failure;
	entelea_result_t r;

	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK, "persist older NONE");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK, "persist newer PENDING");
	entelea_trial_test_reset();

	meta_read_base = entelea_store_backend;
	failure = meta_read_base;
	failure.read = fail_second_meta_copy;
	zassert_equal(entelea_storage_mount(&failure), ENTELEA_OK);
	r = entelea_trial_gate(0, dg);
	zassert_equal(entelea_storage_mount(&meta_read_base), ENTELEA_OK);
	zassert_equal(r, ENTELEA_E_IO, "unreadable newer copy is not older NONE");
	assert_unknown_closed("one unreadable copy");
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM,
		      "explicit recovery after the backend becomes readable");
}

ZTEST(framework_trial, test_no_fallback_to_stale_corrupt_copy)
{
	uint8_t dg[32] = {0};
	entelea_trial_candidate_t c = {.slot = 0, .budget_steps = TEST_BUDGET};

	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK, "persist older NONE");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK, "persist newer PENDING");
	zassert_equal(entelea_storage_meta_corrupt_test(1), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_gate(0, dg), ENTELEA_E_IO,
		      "damaged newer copy is not older NONE");
	assert_unknown_closed("one corrupt copy");
}

ZTEST(framework_trial, test_single_or_different_valid_copies_are_ambiguous)
{
	uint8_t dg[32] = {0};
	uint8_t rec[141] = {3, ENTELEA_TRIAL_NONE};

	zassert_equal(entelea_storage_meta_write(rec, sizeof(rec)), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_gate(0, dg), ENTELEA_E_IO,
		      "single older-format copy is not unambiguously current");
	assert_unknown_closed("single valid copy");

	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	rec[1] = ENTELEA_TRIAL_PENDING_CONFIRM;
	rec[4] = 1;
	zassert_equal(entelea_storage_meta_write(rec, sizeof(rec)), ENTELEA_OK);
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_gate(0, dg), ENTELEA_E_IO,
		      "different valid copies are an interrupted update");
	assert_unknown_closed("different valid copies");
}

ZTEST(framework_trial, test_interrupted_mirrored_flush_stays_closed)
{
	uint8_t dg[32] = {0};
	entelea_trial_candidate_t c = {.slot = 0, .budget_steps = TEST_BUDGET};
	entelea_store_ops_t failure;
	entelea_result_t r;

	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	meta_read_base = entelea_store_backend;
	failure = meta_read_base;
	failure.erase_off = fail_second_meta_erase;
	meta_erases = 0;
	zassert_equal(entelea_storage_mount(&failure), ENTELEA_OK);
	r = entelea_trial_flush();
	zassert_equal(entelea_storage_mount(&meta_read_base), ENTELEA_OK);
	zassert_equal(r, ENTELEA_E_IO, "second-copy write failure is propagated");
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_gate(0, dg), ENTELEA_E_IO,
		      "interrupted mirrored update must not choose an older state");
	assert_unknown_closed("interrupted mirrored flush");
}

/* RF1 #9（G4）/ F4：v3 记录布局与纪元延续；坏记录（版本/域外/零纪元/零步数/
 * 步数超上限/权限列表文法坏/旧 v2 长度/超长）→ TIMEOUT+UNKNOWN，两槽门拒。 */
ZTEST(framework_trial, test_rf1_v3_record_and_bad_records)
{
	entelea_trial_test_reset();
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t cur;
	uint8_t rec[ENTELEA_STORAGE_META_MAX];
	uint8_t good[141];
	uint16_t len = 0;

	memset(&c, 0, sizeof(c));
	c.digest[0] = 0xC4;
	c.digest[31] = 0x4C;
	set_perm(&c, "pwm:set:1");
	c.budget_steps = 5000;
	c.version = 7;
	c.slot = 1;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(rec, sizeof(rec), &len),
		      ENTELEA_OK);
	zassert_equal(len, 141, "v3 记录 141B");
	zassert_equal(rec[0], 3, "ver");
	zassert_equal(rec[1], ENTELEA_TRIAL_PENDING_CONFIRM, "state");
	zassert_equal(rec[2], 7, "version");
	zassert_equal(rec[3], 1, "slot");
	zassert_equal(rec[4], 1, "epoch LE");
	zassert_equal(rec[5] | rec[6] | rec[7], 0, "epoch LE");
	zassert_equal(rec[8], 0x88, "budget LE (5000)");
	zassert_equal(rec[9], 0x13, "budget LE (5000)");
	zassert_equal(rec[10] | rec[11], 0, "budget LE");
	zassert_equal(rec[12], 9, "perm_len");
	zassert_equal(memcmp(rec + 13, "pwm:set:1", 9), 0, "perm 文本");
	zassert_equal(rec[109], 0xC4, "digest 偏移 13+96");
	zassert_equal(rec[140], 0x4C, "digest 末字节");

	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
	zassert_equal(cur.slot, 1);
	zassert_equal(cur.epoch, 1U);
	zassert_equal(cur.budget_steps, 5000U, "步数跨重启");
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK, "NONE 记录携带纪元计数");
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(cur.epoch, 2U, "恢复后纪元不回退");

	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(good, sizeof(good), &len),
		      ENTELEA_OK);
	zassert_equal(len, 141);

	memcpy(rec, good, sizeof(good));
	rec[0] = 2;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("ver!=3");

	memcpy(rec, good, sizeof(good));
	rec[1] = 0x77;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("state 域外");

	memcpy(rec, good, sizeof(good));
	rec[3] = ENTELEA_APP_SLOT_COUNT;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("slot 域外");

	memcpy(rec, good, sizeof(good));
	rec[4] = 0;
	rec[5] = 0;
	rec[6] = 0;
	rec[7] = 0;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("epoch=0");

	memcpy(rec, good, sizeof(good));
	rec[8] = 0;
	rec[9] = 0;
	rec[10] = 0;
	rec[11] = 0;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("budget=0");

	memcpy(rec, good, sizeof(good));
	rec[8] = 0x01; /* 100000001 = 0x05F5E101 */
	rec[9] = 0xE1;
	rec[10] = 0xF5;
	rec[11] = 0x05;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("budget>1e8");

	memcpy(rec, good, sizeof(good));
	rec[12] = ENTELEA_TRIAL_PERM_MAX + 1U;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("perm_len>96");

	memcpy(rec, good, sizeof(good));
	rec[12] = 3;
	memcpy(rec + 13, "zzz", 3);
	restore_from(rec, sizeof(good));
	assert_unknown_closed("perm 文法非法");

	/* v2 旧布局（42B）：不迁移，按坏记录处置。 */
	memset(rec, 0, sizeof(rec));
	rec[0] = 2;
	rec[1] = ENTELEA_TRIAL_PENDING_CONFIRM;
	restore_from(rec, 42);
	assert_unknown_closed("v2 记录");

	memcpy(rec, good, sizeof(good));
	rec[141] = 0;
	restore_from(rec, sizeof(good) + 1U);
	assert_unknown_closed("超长记录");

	/* UNKNOWN 后不能以普通提交丢失未知候选及其纪元。 */
	c.slot = 0;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_STATE);
	assert_unknown_closed("UNKNOWN cannot be replaced");
	entelea_storage_test_reset();
	entelea_trial_test_reset();
}

/* F1（D-B）：授权查询仅在 ACTIVE 且槽与字节相符时授予确认的权限与步数；
 * 试用结束即失效；失败时输出清零。 */
ZTEST(framework_trial, test_grant_only_for_active_matching_candidate)
{
	entelea_trial_test_reset();
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t cur;
	entelea_trial_grant_t g;
	uint8_t dg[32];

	memset(&c, 0, sizeof(c));
	memset(dg, 0x42, sizeof(dg));
	memcpy(c.digest, dg, sizeof(dg));
	set_perm(&c, "gpio:read:0");
	c.budget_steps = 500;
	c.slot = 1;
	zassert_equal(entelea_trial_grant(1, dg, NULL), ENTELEA_E_PARAM);
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_grant(1, dg, &g), ENTELEA_E_PERM,
		      "PENDING 无授权");
	zassert_equal(g.perm_len, 0, "失败时输出清零");
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_grant(1, dg, &g), ENTELEA_OK, "ACTIVE 同字节");
	zassert_equal(g.perm_len, 11U);
	zassert_equal(memcmp(g.perm, "gpio:read:0", 11), 0);
	zassert_equal(g.budget_steps, 500U);
	zassert_equal(entelea_trial_grant(0, dg, &g), ENTELEA_E_PERM,
		      "非候选槽无授权");
	dg[0] ^= 1U;
	zassert_equal(entelea_trial_grant(1, dg, &g), ENTELEA_E_PERM,
		      "换字节无授权");
	dg[0] ^= 1U;
	zassert_equal(entelea_trial_end(), ENTELEA_OK);
	zassert_equal(entelea_trial_grant(1, dg, &g), ENTELEA_E_PERM,
		      "试用结束即失效");
	zassert_equal(g.perm_len, 0);
}

/* admission 字段校验（与生产 submit 共用）：步数域外与权限文法错误在状态
 * 检查之前拒绝，不改变状态。 */
ZTEST(framework_trial, test_admission_field_validation_keeps_state)
{
	entelea_trial_test_reset();
	entelea_trial_candidate_t c;

	memset(&c, 0, sizeof(c));
	c.slot = 0;
	c.budget_steps = 0;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_RANGE, "步数 0");
	c.budget_steps = ENTELEA_TRIAL_STEPS_MAX + 1U;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_RANGE, "步数超上限");
	c.budget_steps = TEST_BUDGET;
	set_perm(&c, "gpio:read:0;");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_PARAM, "末尾空声明");
	set_perm(&c, "gpio:read:0;;pwm:set:1");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_PARAM, "中间空声明");
	set_perm(&c, "gpio:bogus:0");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_PARAM, "未知操作");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_UNINITIALIZED,
		      "字段拒绝不改变状态");
	set_perm(&c, "");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK, "空列表=无权限合法");
}

/* D-E：晋升需 ACTIVE 且第二次确认的同一字节；经 KV 持久，重启后仍有效；撤销
 * 即失效；摘要不符与槽域外被拒。 */
ZTEST(framework_trial, test_stable_promote_persists_and_revokes)
{
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t cur;
	entelea_trial_candidate_t bad;
	entelea_trial_grant_t g;
	uint8_t dg[32];
	uint8_t other[32];

	memset(dg, 0x42, sizeof(dg));
	memset(other, 0x43, sizeof(other));
	memset(&c, 0, sizeof(c));
	memcpy(c.digest, dg, sizeof(dg));
	set_perm(&c, "gpio:read:0");
	c.budget_steps = 500;
	c.slot = 1;

	zassert_equal(entelea_trial_promote(&c), ENTELEA_E_STATE, "无候选不可晋升");
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_promote(&c), ENTELEA_E_STATE, "PENDING 不可晋升");
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK, "持久化 ACTIVE 以验证晋升结束");

	bad = cur;
	bad.budget_steps++;
	zassert_equal(entelea_trial_promote(&bad), ENTELEA_E_PERM, "展示对象不符");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_ACTIVE, "失败保持 ACTIVE");

	zassert_equal(entelea_trial_promote(&cur), ENTELEA_OK, "第二次确认的同一字节");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE, "晋升结束试用");

	zassert_equal(entelea_stable_grant(1, dg, &g), ENTELEA_OK, "稳定记录授权");
	zassert_equal(g.perm_len, 11U);
	zassert_equal(memcmp(g.perm, "gpio:read:0", 11), 0);
	zassert_equal(g.budget_steps, 500U);
	zassert_equal(entelea_stable_grant(1, other, &g), ENTELEA_E_PERM, "摘要不符");
	zassert_equal(entelea_stable_grant(0, dg, &g), ENTELEA_E_PERM, "其他槽无记录");
	zassert_equal(entelea_stable_grant(ENTELEA_APP_SLOT_COUNT, dg, &g),
		      ENTELEA_E_PARAM, "槽域外");

	entelea_trial_test_reset(); /* 模拟重启：稳定记录与试用结束均须持久 */
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE, "晋升须持久结束试用");
	zassert_equal(entelea_stable_grant(1, dg, &g), ENTELEA_OK, "重启后仍有效");

	/* 同字节再次试用至 ACTIVE（已持久）：撤销须同时结束该槽试用，否则试用仍授予。 */
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_confirm(&cur), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	zassert_equal(entelea_trial_grant(1, dg, &g), ENTELEA_OK, "试用 ACTIVE 授权");
	zassert_equal(entelea_stable_revoke(1), ENTELEA_OK);
	zassert_equal(entelea_trial_grant(1, dg, &g), ENTELEA_E_PERM, "撤销后试用不授权");
	zassert_equal(entelea_stable_grant(1, dg, &g), ENTELEA_E_PERM, "撤销即失效");
	zassert_equal(g.perm_len, 0);

	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE, "撤销结束的试用已持久");
	zassert_equal(entelea_stable_revoke(ENTELEA_APP_SLOT_COUNT), ENTELEA_E_PARAM);
}

/* D-E 负测：稳定记录损坏（未知版本、长度不符、权限长度越界）一律 E_PERM 且输出清零。
 * 记录布局与 delivery.c 的 STABLE_REC_LEN 一致（1+32+4+1+96）。 */
ZTEST(framework_trial, test_stable_record_corruption_fails_closed)
{
	uint8_t dg[32];
	uint8_t rec[1 + 32 + 4 + 1 + 96];
	entelea_trial_grant_t g;

	memset(dg, 0x42, sizeof(dg));
	memset(rec, 0, sizeof(rec));
	rec[0] = 2; /* 未知版本 */
	memcpy(rec + 1, dg, sizeof(dg));
	zassert_equal(entelea_kv_save(0x53544230U + 1, rec, sizeof(rec)), ENTELEA_OK);
	zassert_equal(entelea_stable_grant(1, dg, &g), ENTELEA_E_PERM, "未知版本拒绝");
	zassert_equal(g.perm_len, 0);

	rec[0] = 1;
	rec[37] = 97; /* 超出 ENTELEA_TRIAL_PERM_MAX */
	zassert_equal(entelea_kv_save(0x53544230U + 1, rec, sizeof(rec)), ENTELEA_OK);
	zassert_equal(entelea_stable_grant(1, dg, &g), ENTELEA_E_PERM, "权限长度越界拒绝");

	rec[37] = 0;
	zassert_equal(entelea_kv_save(0x53544230U + 1, rec, 10), ENTELEA_OK);
	zassert_equal(entelea_stable_grant(1, dg, &g), ENTELEA_E_PERM, "长度不符拒绝");
	zassert_equal(g.budget_steps, 0U);
}
