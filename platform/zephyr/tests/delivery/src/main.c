/* SPDX-License-Identifier: Apache-2.0 */
/* R12 试用/晋升测试：单候选/内容绑定确认/fail-closed 超时/intake 域外拒。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/appstore.h>
#include <entelea/delivery.h>
#include <entelea/storage.h>

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
	zassert_equal(cur.permissions, 0x0005);
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

ZTEST(framework_trial, test_factory_intake_gates)
{
	entelea_trial_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_factory_candidate_t fc;

	memset(&fc, 0, sizeof(fc));
	fc.payload_kind = 0; /* wasm */
	fc.budget_steps = 100000;
	fc.mem_quota_kb = 64;

	zassert_equal(entelea_factory_intake(NULL, 0), ENTELEA_E_PARAM);
	zassert_equal(entelea_factory_intake(&fc, 0), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM,
		      "intake→PENDING（确认前不生效）");

	/* 未知 payload_kind 拒（不默认解释）。 */
	entelea_trial_test_reset();
	entelea_factory_candidate_t bad = fc;

	bad.payload_kind = 7;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_E_PARAM,
		      "未知 payload 拒");
	/* 预算域外。 */
	entelea_trial_test_reset();
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
	/* Lua payload（kind=1）合法路径。 */
	entelea_trial_test_reset();
	bad = fc;
	bad.payload_kind = 1;
	zassert_equal(entelea_factory_intake(&bad, 0), ENTELEA_OK,
		      "Lua payload 合法（不默认解释为 wasm）");
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
	c.permissions = 0x0301;
	c.version = 9;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_trial_candidate_t before;

	entelea_trial_current(&before);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	entelea_trial_test_reset(); /* 模拟重启丢内存态 */
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM,
		      "PENDING 跨重启存活");
	entelea_trial_candidate_t cur;

	entelea_trial_current(&cur);
	zassert_equal(cur.digest[7], 0x5A);
	zassert_equal(cur.permissions, 0x0301);
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
	uint8_t bad[42];

	zassert_equal(entelea_storage_meta_read(bad, sizeof(bad), NULL),
		      ENTELEA_E_PARAM);
	{
		uint16_t blen = 0;

		zassert_equal(entelea_storage_meta_read(bad, sizeof(bad), &blen),
			      ENTELEA_OK);
		zassert_equal(blen, 42);
		bad[1] = 0x77; /* 域外 state */
		zassert_equal(entelea_storage_meta_write(bad, blen), ENTELEA_OK);
	}
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_TIMEOUT,
		      "坏记录 fail-closed 约束态");
	entelea_trial_current(&cur);
	zassert_equal(cur.slot, ENTELEA_TRIAL_SLOT_UNKNOWN, "候选槽不可知");
	/* 清槽也持久：end→flush→restore=NONE。 */
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

ZTEST_SUITE(framework_trial, NULL, trial_setup, NULL, NULL, NULL);

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
	a.permissions = 0x0001;
	zassert_equal(entelea_trial_submit(&a), ENTELEA_OK);
	entelea_trial_current(&shown_a); /* 界面展示 A */
	/* 确认窗超时；工厂随后推 B（更高权限）。 */
	zassert_equal(entelea_trial_timeout(), ENTELEA_OK);
	b = a;
	b.digest[0] = 0xB2;
	b.permissions = 0x00FF;
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
	c.permissions = 0x0102;
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
	bad.permissions ^= 0x8000U;
	zassert_equal(entelea_trial_confirm(&bad), ENTELEA_E_PERM, "permissions");
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

/* RF1 #9（G4）：slot 域与槽保留——REJECTED/TIMEOUT 期换槽 BUSY（被拒字节
 * 不得因换槽脱离试用门）；NONE 后任意合法槽；intake 绑定 slot。 */
ZTEST(framework_trial, test_rf1_submit_slot_rules)
{
	entelea_trial_test_reset();
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t cur;
	entelea_factory_candidate_t fc;

	memset(&c, 0, sizeof(c));
	c.slot = ENTELEA_APP_SLOT_COUNT;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_PARAM, "slot 域外");
	c.slot = ENTELEA_TRIAL_SLOT_UNKNOWN;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_E_PARAM, "UNKNOWN 不可提交");
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);
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
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_NONE);
	zassert_equal(entelea_factory_intake(&fc, 1), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(cur.slot, 1, "intake 绑定 slot");
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
	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
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

/* RF1 #9（G4）：v2 记录布局与纪元延续；坏记录（版本/域外/零纪元/v1 长度/
 * 超长）→ TIMEOUT+UNKNOWN，两槽门拒。 */
ZTEST(framework_trial, test_rf1_v2_record_and_bad_records)
{
	entelea_trial_test_reset();
	zassert_equal(entelea_storage_test_reset(), ENTELEA_OK);
	entelea_trial_candidate_t c;
	entelea_trial_candidate_t cur;
	uint8_t rec[ENTELEA_STORAGE_META_MAX];
	uint8_t good[42];
	uint16_t len = 0;

	memset(&c, 0, sizeof(c));
	c.digest[0] = 0xC4;
	c.digest[31] = 0x4C;
	c.permissions = 0xA1B2;
	c.version = 7;
	c.slot = 1;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	zassert_equal(entelea_trial_flush(), ENTELEA_OK);
	zassert_equal(entelea_storage_meta_read(rec, sizeof(rec), &len),
		      ENTELEA_OK);
	zassert_equal(len, 42, "v2 记录 42B");
	zassert_equal(rec[0], 2, "ver");
	zassert_equal(rec[1], ENTELEA_TRIAL_PENDING_CONFIRM, "state");
	zassert_equal(rec[2], 7, "version");
	zassert_equal(rec[3], 1, "slot");
	zassert_equal(rec[4], 0xB2, "perm LE");
	zassert_equal(rec[5], 0xA1, "perm LE");
	zassert_equal(rec[6], 1, "epoch LE");
	zassert_equal(rec[7] | rec[8] | rec[9], 0, "epoch LE");
	zassert_equal(rec[10], 0xC4, "digest");
	zassert_equal(rec[41], 0x4C, "digest");

	entelea_trial_test_reset();
	zassert_equal(entelea_trial_restore(), ENTELEA_OK);
	entelea_trial_current(&cur);
	zassert_equal(entelea_trial_state(), ENTELEA_TRIAL_PENDING_CONFIRM);
	zassert_equal(cur.slot, 1);
	zassert_equal(cur.epoch, 1U);
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
	zassert_equal(len, 42);

	memcpy(rec, good, sizeof(good));
	rec[0] = 1;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("ver!=2");

	memcpy(rec, good, sizeof(good));
	rec[1] = 0x77;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("state 域外");

	memcpy(rec, good, sizeof(good));
	rec[3] = ENTELEA_APP_SLOT_COUNT;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("slot 域外");

	memcpy(rec, good, sizeof(good));
	rec[6] = 0;
	rec[7] = 0;
	rec[8] = 0;
	rec[9] = 0;
	restore_from(rec, sizeof(good));
	assert_unknown_closed("epoch=0");

	/* v1 布局（ver=1|state|version|rsv|perm16|digest32，38B）：不迁移。 */
	memset(rec, 0, sizeof(rec));
	rec[0] = 1;
	rec[1] = ENTELEA_TRIAL_PENDING_CONFIRM;
	restore_from(rec, 4 + 2 + 32);
	assert_unknown_closed("v1 记录");

	memcpy(rec, good, sizeof(good));
	rec[42] = 0;
	restore_from(rec, sizeof(good) + 1U);
	assert_unknown_closed("超长记录");

	/* UNKNOWN 后：任一合法槽可提交新候选（残余见合同）。 */
	c.slot = 0;
	zassert_equal(entelea_trial_submit(&c), ENTELEA_OK);
	entelea_storage_test_reset();
	entelea_trial_test_reset();
}
