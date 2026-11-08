/* SPDX-License-Identifier: Apache-2.0 */
/* R06 网络机制/依赖状态机测试（fault-class：断链/恢复滞回/部署守卫/
 * 租约竞态+席位视图/幂等回放+unknown 不缓存/恢复风暴/独立 App 判定）。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/netmon.h>
#include <entelea/safety.h>
#include <entelea/wdt.h>

static void *net_setup(void)
{
	entelea_net_test_reset();
	entelea_safety_test_reset();
	entelea_wdt_test_reset();
	return NULL;
}

/* ---- 心跳监视：断链/恢复滞回/部署守卫 ---- */
ZTEST(framework_net, test_linkmon_hysteresis_and_guard)
{
	/* 初始 down。 */
	zassert_false(entelea_net_link_up());
	/* 单次心跳不够（滞回 2）。 */
	entelea_net_hb_host(100);
	zassert_false(entelea_net_link_up(), "单次不恢复");
	entelea_net_hb_host(1100);
	zassert_true(entelea_net_link_up(), "连续 2 次恢复");
	/* 超时断链（>6×1000ms）。 */
	entelea_net_linkmon_tick(1100 + 6 * 1000 + 1);
	zassert_false(entelea_net_link_up(), "超时断链");
	/* 恢复后心跳序列重置（再单次不够）。 */
	entelea_net_hb_host(9000);
	zassert_false(entelea_net_link_up());
	entelea_net_hb_host(9100);
	zassert_true(entelea_net_link_up());
	/* 从未有心跳：tick 不误报 down→up（have_host_hb 守卫）。 */
	entelea_net_test_reset();
	entelea_net_linkmon_tick(999999);
	zassert_false(entelea_net_link_up());

	/* 部署守卫：standalone 断链不迁移安全态（仅 orchestrated 迁移）。 */
	entelea_net_test_reset();
	entelea_safety_test_reset();
	/* standalone 模式经 prov 缺省（未装载 = orchestrated）——直接测
	 * orchestrated 路径：恢复 → set_link(true)。 */
	static const entelea_out_channel_t ch = {
		.uid = "netg", .kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false},
		.fault = {.b = false},
	};
	zassert_equal(entelea_safety_register_channel(&ch), ENTELEA_OK);
	zassert_equal(entelea_safety_poweron_init(), ENTELEA_OK);
	entelea_ch_state_t st;

	zassert_equal(entelea_safety_channel_state("netg", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_POWERON);
	entelea_net_hb_host(100);
	entelea_net_hb_host(1100);
	zassert_true(entelea_net_link_up());
	zassert_equal(entelea_safety_channel_state("netg", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_ACTIVE, "orchestrated: 恢复→ACTIVE");
	entelea_net_linkmon_tick(1100 + 6001);
	zassert_equal(entelea_safety_channel_state("netg", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_LINKLOSS, "orchestrated: 断链→LINKLOSS");
}

/* ---- 租约：准入/续期/过期/席位视图 ---- */
ZTEST(framework_net, test_lease_admission_and_views)
{
	uint32_t id1 = 0, id2 = 0;
	uint64_t exp = 0;

	zassert_equal(entelea_net_lease_acquire("", 1000, &id1, &exp),
		      ENTELEA_E_PARAM, "空 holder 拒");
	zassert_equal(entelea_net_lease_acquire("host-a", 1000, &id1, &exp),
		      ENTELEA_OK);
	zassert_equal(id1, 1);
	/* 他人持有拒（回填归因）。 */
	zassert_equal(entelea_net_lease_acquire("host-b", 2000, &id2, &exp),
		      ENTELEA_E_STATE);
	zassert_equal(id2, id1, "拒时回填当前租约 id");
	/* 同 holder 幂等续期（id 不变）。 */
	zassert_equal(entelea_net_lease_acquire("host-a", 5000, &id2, &exp),
		      ENTELEA_OK);
	zassert_equal(id2, id1, "续期 id 不变");
	/* 续期后窗口顺延至 5000+TTL=15000；过期后（>15000）新授予 id
	 * 递增不复用。 */
	zassert_equal(entelea_net_lease_acquire("host-b", 12000, &id2, &exp),
		      ENTELEA_E_STATE, "续期内他人仍拒");
	zassert_equal(entelea_net_lease_acquire("host-b", 15000 + 1, &id2, &exp),
		      ENTELEA_OK);
	zassert_equal(id2, id1 + 1, "过期重授新 id");
	/* held_by 视图。 */
	zassert_true(entelea_net_lease_held_by("host-b", 12000));
	zassert_false(entelea_net_lease_held_by("host-a", 12000));
	/* 他人 release 拒；holder release 幂等。 */
	zassert_equal(entelea_net_lease_release("host-a", 12000), ENTELEA_E_STATE);
	zassert_equal(entelea_net_lease_release("host-b", 12000), ENTELEA_OK);
	zassert_equal(entelea_net_lease_release("host-b", 12000), ENTELEA_OK,
		      "无租约幂等空操作");

	/* 本地席位：acquire 拒 + held_by 恒 false（视图，不改在册）。 */
	zassert_equal(entelea_net_lease_acquire("host-c", 13000, &id1, &exp),
		      ENTELEA_OK);
	zassert_true(entelea_net_lease_held_by("host-c", 13001));
	zassert_equal(entelea_safety_set_arb(ENTELEA_ARB_LOCAL), ENTELEA_OK);
	zassert_false(entelea_net_lease_held_by("host-c", 13002),
		      "本地席位 held_by 恒 false");
	zassert_equal(entelea_net_lease_acquire("host-c", 13003, &id2, &exp),
		      ENTELEA_E_STATE, "本地席位 acquire 拒");
	zassert_equal(entelea_safety_set_arb(ENTELEA_ARB_REMOTE), ENTELEA_OK);
	zassert_true(entelea_net_lease_held_by("host-c", 13004),
		     "释放后旧租约若未过期仍有效（视图语义）");
}

/* ---- 幂等缓存：回放/LRU/unknown 不缓存 ---- */
static int exec_count;
static uint8_t last_req;

static entelea_result_t fake_exec(const uint8_t *req, size_t len,
				  uint8_t *out, size_t *out_len)
{
	exec_count++;
	last_req = req[0];
	out[0] = (uint8_t)(req[0] + 1);
	*out_len = 1;
	return ENTELEA_OK;
}

static entelea_result_t pending_exec(const uint8_t *req, size_t len,
				     uint8_t *out, size_t *out_len)
{
	exec_count++;
	out[0] = 0;
	*out_len = 1;
	return ENTELEA_PENDING;
}

ZTEST(framework_net, test_idem_replay_and_unknown_nocache)
{
	uint8_t req = 10, rep[16];
	size_t rlen = sizeof(rep);
	bool replayed = true;

	exec_count = 0;
	zassert_equal(entelea_net_idem_run("k1", &req, 1, fake_exec, rep, &rlen,
					   &replayed),
		      ENTELEA_OK);
	zassert_false(replayed, "首执行");
	zassert_equal(exec_count, 1);
	zassert_equal(rep[0], 11);
	/* 同 idem 重放：不重执行、回放原回执。 */
	req = 99; /* 换请求体也不重执行（idem 键权威） */
	rlen = sizeof(rep);
	zassert_equal(entelea_net_idem_run("k1", &req, 1, fake_exec, rep, &rlen,
					   &replayed),
		      ENTELEA_OK);
	zassert_true(replayed, "命中回放");
	zassert_equal(exec_count, 1, "不重执行");
	zassert_equal(rep[0], 11, "回放原回执");
	zassert_equal(last_req, 10, "exec 未被调用（请求体未变）");
	/* unknown/PENDING 不缓存：同 idem 重执行。 */
	exec_count = 0;
	rlen = sizeof(rep);
	zassert_equal(entelea_net_idem_run("k2", &req, 1, pending_exec, rep,
					   &rlen, &replayed),
		      ENTELEA_PENDING, "PENDING 如实返回");
	zassert_false(replayed);
	rlen = sizeof(rep);
	zassert_equal(entelea_net_idem_run("k2", &req, 1, pending_exec, rep,
					   &rlen, &replayed),
		      ENTELEA_PENDING);
	zassert_equal(exec_count, 2, "unknown 非幂等不缓存：重执行");
	zassert_false(replayed);
	/* LRU 驱逐：填满 4 槽后最旧被逐。 */
	uint8_t r = 1;

	for (int i = 0; i < ENTELEA_NET_IDEM_CACHE + 1; i++) {
		char key[8];

		snprintf(key, sizeof(key), "L%d", i);
		rlen = sizeof(rep);
		r = (uint8_t)i;
		zassert_equal(entelea_net_idem_run(key, &r, 1, fake_exec, rep,
						   &rlen, &replayed),
			      ENTELEA_OK);
		zassert_false(replayed);
	}
	/* L0 已被逐（重执行）；k1 也被逐；L4 最新（回放）。 */
	exec_count = 0;
	rlen = sizeof(rep);
	r = 0;
	zassert_equal(entelea_net_idem_run("L0", &r, 1, fake_exec, rep, &rlen,
					   &replayed),
		      ENTELEA_OK);
	zassert_false(replayed, "最旧被逐（重执行）");
	zassert_equal(exec_count, 1);
	rlen = sizeof(rep);
	zassert_equal(entelea_net_idem_run("L4", &r, 1, fake_exec, rep, &rlen,
					   &replayed),
		      ENTELEA_OK);
	zassert_true(replayed, "最新命中");
	/* 参数负测。 */
	zassert_equal(entelea_net_idem_run(NULL, &req, 1, fake_exec, rep, &rlen,
					   &replayed),
		      ENTELEA_E_PARAM);
	char longkey[32];

	memset(longkey, 'x', sizeof(longkey));
	longkey[31] = '\0';
	zassert_equal(entelea_net_idem_run(longkey, &req, 1, fake_exec, rep,
					   &rlen, &replayed),
		      ENTELEA_E_PARAM, "超长 idem 拒");
}

/* ---- 依赖状态机：恢复防抖/风暴/独立判定 ---- */
ZTEST(framework_net, test_dep_state_machine_and_storm)
{
	/* 初始 DOWN。 */
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_DOWN);
	/* 假传输注入：DNS/TLS/云不可用 = bad 观测。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, false, 100),
		      ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_NET), ENTELEA_DEP_DOWN);
	/* 恢复防抖：1 次 good → DEGRADED（不 UP——风暴抑制）。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 200), ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_NET), ENTELEA_DEP_DEGRADED);
	/* 恢复风暴：good/bad 交替 → 永不 UP。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, false, 300), ENTELEA_OK);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 400), ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_NET), ENTELEA_DEP_DEGRADED);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, false, 500), ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_NET), ENTELEA_DEP_DOWN,
		      "bad 立即 DOWN（保守）");
	/* 连续 2 good → UP。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 600), ENTELEA_OK);
	zassert_equal(entelea_dep_report(ENTELEA_DEP_NET, true, 700), ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_NET), ENTELEA_DEP_UP);
	/* 独立判定：model down 不影响 net up（独立 App 判各自依赖）。 */
	zassert_equal(entelea_dep_report(ENTELEA_DEP_MODEL, false, 800),
		      ENTELEA_OK);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_MODEL), ENTELEA_DEP_DOWN);
	zassert_equal(entelea_dep_state(ENTELEA_DEP_NET), ENTELEA_DEP_UP,
		      "model down 不绑死 net");
	/* 云模型不可用 = 智脑依赖 DOWN（辅助功能暂停；独立 App 照常——
	 * 各自查询面）。 */
	zassert_equal(entelea_dep_state(ENTELEA_DEP_APP), ENTELEA_DEP_DOWN);
	zassert_equal(entelea_dep_report((entelea_dep_kind_t)99, true, 900),
		      ENTELEA_E_PARAM, "域外 kind 拒");
	zassert_ok(strcmp(entelea_dep_name(ENTELEA_DEP_MODEL), "model"));
}

ZTEST_SUITE(framework_net, NULL, net_setup, NULL, NULL, NULL);
