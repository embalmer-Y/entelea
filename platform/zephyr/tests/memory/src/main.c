/* SPDX-License-Identifier: Apache-2.0 */
/* R10 经历记忆测试：同意门/有界让位/删除不复活/健康观测。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/memory.h>

static void *mem_setup(void)
{
	entelea_mem_test_reset();
	return NULL;
}

static entelea_mem_entry_t mk(uint64_t t, uint8_t b)
{
	entelea_mem_entry_t e;

	memset(&e, 0, sizeof(e));
	e.t_ms = t;
	e.kind = b;
	e.len = 4;
	e.data[0] = b;
	return e;
}

ZTEST(framework_mem, test_write_consent_and_bounds)
{
	entelea_mem_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_mem_entry_t e = mk(100, 1);

	zassert_equal(entelea_mem_write(false, &e), ENTELEA_E_PERM,
		      "无同意不保存");
	entelea_mem_health_t h;

	entelea_mem_health(&h);
	zassert_equal(h.count, 0, "拒绝路径零写入");
	zassert_equal(entelea_mem_write(true, &e), ENTELEA_OK);
	entelea_mem_health(&h);
	zassert_equal(h.count, 1);

	/* 参数负测。 */
	zassert_equal(entelea_mem_write(true, NULL), ENTELEA_E_PARAM);
	entelea_mem_entry_t bad = mk(1, 1);

	bad.len = 0;
	zassert_equal(entelea_mem_write(true, &bad), ENTELEA_E_PARAM);
	bad.len = ENTELEA_MEM_ENTRY_MAX + 1;
	zassert_equal(entelea_mem_write(true, &bad), ENTELEA_E_PARAM);
}

ZTEST(framework_mem, test_bounded_chain_eviction)
{
	entelea_mem_test_reset(); /* 用例序不保证：自含净基线 */
	for (uint64_t t = 1; t <= ENTELEA_MEM_CHAIN_MAX + 5; t++) {
		entelea_mem_entry_t e = mk(t, (uint8_t)t);

		zassert_equal(entelea_mem_write(true, &e), ENTELEA_OK);
	}
	entelea_mem_health_t h;

	entelea_mem_health(&h);
	zassert_equal(h.count, ENTELEA_MEM_CHAIN_MAX, "有界");
	zassert_equal(h.evicted, 5, "让位计数");
	/* 保留最近（旧→新读）。 */
	entelea_mem_entry_t out[ENTELEA_MEM_CHAIN_MAX];
	size_t n = entelea_mem_read(out, sizeof(out) / sizeof(out[0]));

	zassert_equal(n, ENTELEA_MEM_CHAIN_MAX);
	zassert_equal(out[0].t_ms, 6, "最旧被让位");
	zassert_equal(out[n - 1].t_ms, ENTELEA_MEM_CHAIN_MAX + 5);
}

ZTEST(framework_mem, test_delete_tombstone_no_revival)
{
	entelea_mem_test_reset(); /* 用例序不保证：自含净基线 */
	for (uint64_t t = 10; t <= 30; t += 10) {
		entelea_mem_entry_t e = mk(t, (uint8_t)(t / 10));

		zassert_equal(entelea_mem_write(true, &e), ENTELEA_OK);
	}
	/* 删除中间区间。 */
	zassert_equal(entelea_mem_delete(15, 25), ENTELEA_OK);
	entelea_mem_entry_t out[8];
	size_t n = entelea_mem_read(out, 8);

	zassert_equal(n, 2, "墓碑区间跳过");
	zassert_equal(out[0].t_ms, 10);
	zassert_equal(out[1].t_ms, 30);
	entelea_mem_health_t h;

	entelea_mem_health(&h);
	zassert_equal(h.tombstoned, 1);
	/* 墓碑不可逆（不撤回已删除）。 */
	zassert_equal(entelea_mem_delete(0, 0), ENTELEA_OK); /* 换区间 */
	entelea_mem_entry_t later = mk(50, 9);

	zassert_equal(entelea_mem_write(true, &later), ENTELEA_OK);
	n = entelea_mem_read(out, 8);
	zassert_equal(out[n - 1].t_ms, 50, "墓碑外新条目正常");
	zassert_equal(entelea_mem_delete(100, 1), ENTELEA_E_PARAM,
		      "逆区间拒");
	/* review P1 负测：第二次 delete 不复活第一次删除的区间。 */
	entelea_mem_test_reset();
	for (uint64_t t = 10; t <= 30; t += 10) {
		entelea_mem_entry_t e2 = mk(t, (uint8_t)(t / 10));

		zassert_equal(entelea_mem_write(true, &e2), ENTELEA_OK);
	}
	zassert_equal(entelea_mem_delete(15, 25), ENTELEA_OK); /* 隐 t=20 */
	zassert_equal(entelea_mem_delete(0, 5), ENTELEA_OK); /* 新区间 */
	n = entelea_mem_read(out, 8);
	zassert_equal(n, 2, "两条可见");
	zassert_equal(out[0].t_ms, 10);
	zassert_equal(out[1].t_ms, 30);
	/* t=20 保持隐藏（墓碑累计不覆盖）。 */
	for (size_t i = 0; i < n; i++) {
		zassert_not_equal(out[i].t_ms, 20, "首区间不复活");
	}
}

/* ---- 墓碑容量边界（计划符合性审查 P2）：满时不得复活任何已删区间 ---- */
ZTEST(framework_mem, test_tombstone_capacity_no_revival)
{
	entelea_mem_test_reset(); /* 用例序不保证：自含净基线 */
	/* 9 条不同时间戳经历：t=10..90（步进 10）。 */
	for (uint64_t t = 10; t <= 90; t += 10) {
		entelea_mem_entry_t e = {.t_ms = t, .len = 1};

		zassert_equal(entelea_mem_write(true, &e), ENTELEA_OK);
	}
	/* 8 个互不覆盖墓碑（t=10..80 各自单点）→ 槽满。 */
	for (uint64_t t = 10; t <= 80; t += 10) {
		zassert_equal(entelea_mem_delete(t, t), ENTELEA_OK);
	}
	/* 第 9 个不覆盖任何既有区间的删除 → E_NOMEM 显式拒
	 * （修复前：覆盖 from 最小槽 → t=10 复活）。 */
	zassert_equal(entelea_mem_delete(90, 90), ENTELEA_E_NOMEM,
		      "满 + 无覆盖：显式拒（不复活）");
	entelea_mem_entry_t out[16];
	size_t n = entelea_mem_read(out, 16);

	zassert_equal(n, 1, "只余 t=90");
	zassert_equal(out[0].t_ms, 90);
	/* 覆盖式删除恒可成功：大区间覆盖一切既有墓碑 → 复用槽 OK。 */
	zassert_equal(entelea_mem_delete(0, UINT64_MAX), ENTELEA_OK,
		      "全区间覆盖既有 → 复用槽成功");
	n = entelea_mem_read(out, 16);
	zassert_equal(n, 0, "全删后链空（覆盖面不丢）");
	/* 健康计数：8 次 + 1 次覆盖复用 = 9；E_NOMEM 拒绝路径不计。 */
	entelea_mem_health_t h;

	entelea_mem_health(&h);
	zassert_equal(h.tombstoned, 9);
}

ZTEST_SUITE(framework_mem, NULL, mem_setup, NULL, NULL, NULL);
