/* SPDX-License-Identifier: Apache-2.0 */
/* R11 情感/适应测试：单写者演进/确定性回放/版本环回滚/情感关闭独立。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/affect.h>

static void *affect_setup(void)
{
	entelea_affect_test_reset();
	return NULL;
}

/* helper wrapper（ztest 宏内不便直接循环调用带断言——独立返回码面） */
static int entea_unused_tick(uint32_t dt, const int8_t st[ENTELEA_AFFECT_LEVELS])
{
	return entelea_affect_tick(dt, st) == ENTELEA_OK ? 0 : 1;
}

static void tick_n(uint32_t dt, const int8_t st[ENTELEA_AFFECT_LEVELS], int n)
{
	for (int i = 0; i < n; i++) {
		zassert_equal(entea_unused_tick(dt, st), 0);
	}
}

ZTEST(framework_affect, test_tick_evolution_and_determinism)
{
	entelea_affect_test_reset(); /* 用例序不保证：自含净基线 */
	int8_t st[ENTELEA_AFFECT_LEVELS] = {0};

	zassert_equal(entelea_affect_tick(0, st), ENTELEA_E_PARAM, "dt=0 拒");
	zassert_equal(entelea_affect_tick(1000, NULL), ENTELEA_E_PARAM);
	entelea_affect_state_t a;

	entelea_affect_get(&a);
	zassert_equal(a.level[0], 50, "初始中位");

	/* 升/降/回中。 */
	st[0] = 10; st[1] = -10; st[2] = 0;
	tick_n(1000, st, 3);
	entelea_affect_get(&a);
	zassert_equal(a.level[0], 53, "正刺激升");
	zassert_equal(a.level[1], 47, "负刺激降");
	zassert_equal(a.level[2], 50, "零刺激中位保持");

	/* 确定性：同输入同状态重放一致。 */
	entelea_affect_state_t snap;

	entelea_affect_get(&snap);
	tick_n(1000, st, 2);
	entelea_affect_state_t run1;

	entelea_affect_get(&run1);
	entelea_affect_test_reset();
	/* 重放到同起点（3+2=5 次 tick）。 */
	tick_n(1000, st, 3);
	tick_n(1000, st, 2);
	entelea_affect_state_t run2;

	entelea_affect_get(&run2);
	zassert_ok(memcmp(&run1, &run2, sizeof(run1)), "确定性回放一致");

	/* 步长饱和（dt 超大——变化有界）。 */
	entelea_affect_test_reset();
	st[0] = 100;
	zassert_equal(entelea_affect_tick(100000, st), ENTELEA_OK);
	entelea_affect_get(&a);
	zassert_equal(a.level[0], 60, "步长饱和 10");

	/* 域界（0/100 钳制）：50 起步 × 饱和步 10 × 5 tick = 100。 */
	entelea_affect_test_reset();
	st[0] = 100;
	tick_n(100000, st, 5);
	entelea_affect_get(&a);
	zassert_equal(a.level[0], 100, "上限钳制");
}

ZTEST(framework_affect, test_adapt_versions_and_rollback)
{
	entelea_affect_test_reset(); /* 用例序不保证：自含净基线 */
	uint8_t w[ENTELEA_AFFECT_LEVELS] = {1, 2, 3, 4};
	entelea_adapt_params_t p;

	zassert_equal(entelea_adapt_rollback(&p), ENTELEA_E_STATE,
		      "无历史不静默回滚");
	zassert_equal(entelea_adapt_commit(w, &p), ENTELEA_OK);
	zassert_equal(p.version, 1);
	w[0] = 9;
	zassert_equal(entelea_adapt_commit(w, &p), ENTELEA_OK);
	zassert_equal(p.version, 2);
	entelea_adapt_current(&p);
	zassert_equal(p.weight[0], 9, "当前=最新");
	/* 回滚到 v1。 */
	zassert_equal(entelea_adapt_rollback(&p), ENTELEA_OK);
	zassert_equal(p.version, 1);
	zassert_equal(p.weight[0], 1, "回滚恢复旧参数");
	/* 再提交=新版本（不复活已回滚 v2）。 */
	w[0] = 7;
	zassert_equal(entelea_adapt_commit(w, &p), ENTELEA_OK);
	zassert_equal(p.version, 3, "版本单调（回滚后新提交不复用 v2）");
	/* 环满让最旧（8 环）。 */
	for (int i = 0; i < ENTELEA_ADAPT_HISTORY + 2; i++) {
		w[0] = (uint8_t)i;
		zassert_equal(entelea_adapt_commit(w, &p), ENTELEA_OK);
	}
	entelea_adapt_current(&p);
	zassert_equal(p.weight[0], ENTELEA_ADAPT_HISTORY + 1, "最新保留");
	/* 历史仍有 8 版可回滚。 */
	for (int i = 0; i < ENTELEA_ADAPT_HISTORY - 1; i++) {
		zassert_equal(entelea_adapt_rollback(&p), ENTELEA_OK);
	}
	zassert_equal(entelea_adapt_rollback(&p), ENTELEA_E_STATE,
		      "回到最旧后再回=拒绝");
	zassert_equal(entelea_adapt_commit(NULL, &p), ENTELEA_E_PARAM);
}

ZTEST(framework_affect, test_emotion_off_independent)
{
	entelea_affect_test_reset(); /* 用例序不保证：自含净基线 */
	/* 情感关闭（零 stimulus）→ 回中位但不破坏任何配置面（本层无
	 * 权限/安全写路径——独立性由结构保证）。 */
	int8_t zero[ENTELEA_AFFECT_LEVELS] = {0};

	/* 先拉高（饱和步 × 5 = 100）。 */
	int8_t up[ENTELEA_AFFECT_LEVELS] = {100, 100, 100, 100};

	tick_n(100000, up, 5);
	entelea_affect_state_t a;

	entelea_affect_get(&a);
	zassert_equal(a.level[0], 100);
	/* 关闭（零刺激）——饱和步回落中位（100→50 需 5 步 × 10）。 */
	tick_n(100000, zero, 5);
	entelea_affect_get(&a);
	zassert_equal(a.level[0], 50, "零刺激回中位");
	/* 适应面不受情感影响（独立）。 */
	uint8_t w[4] = {5, 5, 5, 5};
	entelea_adapt_params_t p;

	zassert_equal(entelea_adapt_commit(w, &p), ENTELEA_OK);
	zassert_equal(p.weight[0], 5, "适应独立于情感状态");
}

ZTEST_SUITE(framework_affect, NULL, affect_setup, NULL, NULL, NULL);
