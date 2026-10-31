/* SPDX-License-Identifier: Apache-2.0 */
/* framework.conc（DEC-43 锁收口回归）：SMP 真并行下的并发不变量常设守卫。
 *   test_01 对齐双冲预算——两线程信号量栅栏同步起跑，检查-提交原子化后
 *          超限必须为 0（真 SMP 板上若锁回归破洞，此断言即红）；
 *   test_02 迁移×提交×单通道恢复三方压力——表完整/终态一致/无死锁；
 *   test_03 pubq 并发完整性——锁内出队 + 锁外发送：序号单调（环无损）
 *          + delivered + dropped == pushed 会计闭合。
 * 注：native_sim 上 test_01 的判别力受唤醒串行化限制（wamrdemo 实验补充
 * 已留档）——本套件的完整判别力在真 SMP 板（xiao_esp32s3 双核）上兑现。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>
#include <ts/hal.h>
#include <ts/net.h>
#include <ts/power.h>
#include <ts/safety.h>
#include "../../../module/tessera/src/net/internal.h"

/* ---- 公共：栅栏与线程样板 -------------------------------------------------- */

static struct k_sem bar_ready; /* 主线程收 N */
static struct k_sem bar_go;    /* N 工作线程各收 1 */

static void barrier_setup(void)
{
	k_sem_init(&bar_ready, 0, 2);
	k_sem_init(&bar_go, 0, 2);
}

static void barrier_release(void)
{
	k_sem_take(&bar_ready, K_FOREVER);
	k_sem_take(&bar_ready, K_FOREVER);
	k_sem_give(&bar_go);
	k_sem_give(&bar_go);
}

/* ---- test_01：对齐双冲预算（DEC-43 收口回归）-------------------------------- */

#define T01_ROUNDS 3000u

struct t01_arg {
	ts_ctx_t ctx;
	uint8_t slot;
	uint32_t overshoot;
	uint32_t ok;
	uint32_t range;
	uint32_t other;
};

static void t01_entry(void *p1, void *p2, void *p3)
{
	struct t01_arg *a = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (uint32_t i = 0; i < T01_ROUNDS; i++) {
		k_sem_give(&bar_ready);
		k_sem_take(&bar_go, K_FOREVER);

		ts_res_t r = ts_power_request(a->ctx, a->slot, true, 600);

		if (r == TS_OK) {
			a->ok++;
			ts_power_budget_t b;

			ts_power_budget_snapshot(&b);
			if (b.used_ma > b.budget_ma) {
				a->overshoot++;
			}
			(void)ts_power_request(a->ctx, a->slot, false, 0);
		} else if (r == TS_E_RANGE) {
			a->range++;
		} else {
			a->other++;
		}
	}
}

static K_THREAD_STACK_DEFINE(t01_stack_a, 16384);
static K_THREAD_STACK_DEFINE(t01_stack_b, 16384);
static struct k_thread t01_th_a, t01_th_b;

static ts_ctx_t make_ctx(void)
{
	ts_ctx_t ctx;
	ts_perm_table_t t;

	ts_perm_table_init(&t);
	zassert_equal(ts_perm_parse("power:set:0-3", &t), TS_OK);
	zassert_equal(ts_hal_bind_context(&ctx, 7, &t), TS_OK);
	return ctx;
}

ZTEST(framework_conc, test_01_aligned_budget_burst)
{
	static const ts_pwr_slot_t s0 = {.uid = "cb0", .current_limit_ma = 600};
	static const ts_pwr_slot_t s1 = {.uid = "cb1", .current_limit_ma = 600};
	ts_ctx_t ctx = make_ctx();

	ts_safety_test_reset();
	ts_power_test_reset();
	zassert_equal(ts_power_register_slot(&s0), TS_OK);
	zassert_equal(ts_power_register_slot(&s1), TS_OK);
	ts_power_test_set_budget(1000); /* 单请 600 合法；双开 1200 必超限 */
	ts_safety_set_link(true);

	struct t01_arg a = {.ctx = ctx, .slot = 0};
	struct t01_arg b = {.ctx = ctx, .slot = 1};

	barrier_setup();
	k_thread_create(&t01_th_a, t01_stack_a, K_THREAD_STACK_SIZEOF(t01_stack_a),
			t01_entry, &a, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_create(&t01_th_b, t01_stack_b, K_THREAD_STACK_SIZEOF(t01_stack_b),
			t01_entry, &b, NULL, NULL, 5, 0, K_NO_WAIT);
	for (uint32_t r = 0; r < T01_ROUNDS; r++) {
		barrier_release();
	}
	zassert_equal(k_thread_join(&t01_th_a, K_SECONDS(60)), 0, "A join（无死锁）");
	zassert_equal(k_thread_join(&t01_th_b, K_SECONDS(60)), 0, "B join（无死锁）");
	printk("[conc-01] 对齐双冲 %u 轮：超限 %u+%u（锁后必须 0）；"
	       "ok=%u/%u range=%u/%u other=%u/%u\n",
	       (unsigned)T01_ROUNDS, a.overshoot, b.overshoot,
	       a.ok, b.ok, a.range, b.range, a.other, b.other);
	zassert_equal(a.overshoot + b.overshoot, 0, "预算超限（检查-提交原子性破洞）");
	zassert_equal(a.ok + b.ok + a.range + b.range + a.other + b.other,
		      2u * T01_ROUNDS, "全部轮次有结果");
	zassert_equal(a.other + b.other, 0, "无意外返回码");
}

/* ---- test_02：迁移 × 提交 × 单通道恢复压力（不变量）------------------------- */

#define T02_FLAP  5000u
#define T02_COMM  10000u
#define T02_FR    2000u

static struct t02_stat {
	uint32_t ok;
	uint32_t state;
} t02_comm_stat;

static void t02_flap_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (uint32_t i = 0; i < T02_FLAP; i++) {
		ts_safety_set_link((i % 8) != 7); /* 偶尔保持 down 数拍，收尾 true */
	}
	ts_safety_set_link(true); /* 终态统一 ACTIVE */
}

static void t02_commit_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (uint32_t i = 0; i < T02_COMM; i++) {
		ts_res_t r = ts_safety_commit("cg0",
			      (ts_out_value_t){.b = (i & 1) != 0});

		if (r == TS_OK) {
			t02_comm_stat.ok++;
		} else if (r == TS_E_STATE) {
			t02_comm_stat.state++;
		}
	}
}

static void t02_fr_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (uint32_t i = 0; i < T02_FR; i++) {
		(void)ts_safety_force_channel_fault("cg0");
		(void)ts_safety_channel_recover("cg0");
	}
}

static K_THREAD_STACK_DEFINE(t02_stack_f, 8192);
static K_THREAD_STACK_DEFINE(t02_stack_c, 8192);
static K_THREAD_STACK_DEFINE(t02_stack_r, 8192);
static struct k_thread t02_th_f, t02_th_c, t02_th_r;

ZTEST(framework_conc, test_02_migration_commit_stress)
{
	static const ts_out_ch_t ch = {
		.uid = "cg0", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
	};

	ts_safety_test_reset();
	zassert_equal(ts_safety_register_channel(&ch), TS_OK);
	ts_safety_set_link(true);
	memset(&t02_comm_stat, 0, sizeof(t02_comm_stat));

	k_thread_create(&t02_th_f, t02_stack_f, K_THREAD_STACK_SIZEOF(t02_stack_f),
			t02_flap_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_create(&t02_th_c, t02_stack_c, K_THREAD_STACK_SIZEOF(t02_stack_c),
			t02_commit_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_create(&t02_th_r, t02_stack_r, K_THREAD_STACK_SIZEOF(t02_stack_r),
			t02_fr_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	zassert_equal(k_thread_join(&t02_th_f, K_SECONDS(60)), 0, "flap join");
	zassert_equal(k_thread_join(&t02_th_c, K_SECONDS(60)), 0, "commit join");
	zassert_equal(k_thread_join(&t02_th_r, K_SECONDS(60)), 0, "fr join");

	/* 终态不变量：链路 up + 无锁存 → ACTIVE；表完整；提交路径健康 */
	ts_ch_state_t st;

	zassert_equal(ts_safety_channel_state("cg0", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE, "终态 = ACTIVE（迁移无残留）");
	ts_safety_summary_t sum;

	zassert_equal(ts_safety_summary(&sum), TS_OK);
	zassert_equal(sum.channels, 1, "通道表未腐蚀");
	zassert_equal(ts_safety_commit("cg0", (ts_out_value_t){.b = true}), TS_OK,
		      "压力后提交路径健康（锁未损）");
	zassert_equal(t02_comm_stat.ok + t02_comm_stat.state, T02_COMM,
		      "提交计数闭合（OK 或 E_STATE——断链/故障窗口内拒绝属正常）");
	printk("[conc-02] 提交 %u 次：ok=%u state=%u（窗口内拒绝为正常语义）\n",
	       (unsigned)T02_COMM, t02_comm_stat.ok, t02_comm_stat.state);
}

/* ---- test_03：pubq 并发完整性 ------------------------------------------------ */

#define T03_PUSH 4000u

static uint8_t t03_payload[8];
static uint32_t t03_last_seq;
static uint32_t t03_delivered;
static bool t03_order_ok = true;

static ts_res_t t03_fake_open(void)
{
	return TS_OK;
}

static void t03_fake_close(void)
{
}

static bool t03_fake_up(void)
{
	return true;
}

static ts_res_t t03_fake_pub(const char *key, const uint8_t *p, uint32_t l,
			     ts_net_qos_t qos)
{
	ARG_UNUSED(key);
	ARG_UNUSED(l);
	ARG_UNUSED(qos);
	uint32_t seq;

	memcpy(&seq, p, 4);
	if (seq <= t03_last_seq && t03_delivered > 0) {
		t03_order_ok = false; /* 同 key FIFO：序号必须严格递增 */
	}
	t03_last_seq = seq;
	t03_delivered++;
	return TS_OK;
}

static const ts_net_transport_t t03_fake_t = {
	.open = t03_fake_open, .close = t03_fake_close,
	.publish = t03_fake_pub, .is_up = t03_fake_up,
};

static void t03_push_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (uint32_t i = 1; i <= T03_PUSH; i++) {
		memcpy(t03_payload, &i, 4);
		(void)ts_net_pubq_push("tessera/n/c/t/telemetry", t03_payload,
				       sizeof(t03_payload));
	}
}

static K_THREAD_STACK_DEFINE(t03_stack, 8192);
static struct k_thread t03_th;

ZTEST(framework_conc, test_03_pubq_integrity)
{
	ts_net_test_reset(); /* 内含 pubq 复位（session.c） */
	(void)ts_net_set_ids("n", "c");
	ts_net_set_transport(&t03_fake_t);
	zassert_equal(ts_net_session_poll(0), TS_NET_CONNECTED, "会话拉起");

	memset(&t03_last_seq, 0, sizeof(t03_last_seq));
	t03_delivered = 0;
	t03_order_ok = true;

	k_thread_create(&t03_th, t03_stack, K_THREAD_STACK_SIZEOF(t03_stack),
			t03_push_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	uint32_t guard = 0;

	while (t03_delivered + 8u < T03_PUSH && guard++ < 200000u) {
		ts_net_pubq_flush();
		if ((guard % 1000u) == 0u) {
			k_msleep(1); /* 让生产者推进 */
		}
	}
	zassert_equal(k_thread_join(&t03_th, K_SECONDS(30)), 0, "push join");
	ts_net_pubq_flush(); /* 清尾 */
	uint32_t dropped = ts_net_pubq_dropped();
	printk("[conc-03] push=%u delivered=%u dropped=%u 序号单调=%s\n",
	       (unsigned)T03_PUSH, t03_delivered, dropped,
	       t03_order_ok ? "yes" : "NO");
	zassert_true(t03_order_ok, "序号严格递增（环形结构无损）");
	zassert_equal(t03_delivered + dropped, T03_PUSH, "会计闭合（无丢失无重复）");
}

ZTEST_SUITE(framework_conc, NULL, NULL, NULL, NULL, NULL);
