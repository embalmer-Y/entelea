/* SPDX-License-Identifier: Apache-2.0 */
/* ts-core L1/L4 测试（LLD-ts-core §7）：虚拟时钟 / 事件顺序 / WDT 数学 / boot 顺序。 */
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>
#include <ts/core.h>
#include <ts/safety.h>

static uint64_t virt_now;
static uint64_t virt_now_fn(void)
{
	return virt_now;
}
static const ts_time_source_t virt_src = {.now_ms = virt_now_fn};

ZTEST(framework_core, test_time_virtual_clock)
{
	ts_time_test_bind(&virt_src);
	virt_now = 1234;
	zassert_equal(ts_time_ms(), 1234, "virtual clock");
	virt_now = 56789;
	zassert_equal(ts_time_ms(), 56789, "virtual clock advance");
	ts_time_test_bind(NULL);
}

static int order_log[8];
static int order_n;

static void sub_a(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	order_log[order_n++] = 'A';
}
static void sub_b(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	order_log[order_n++] = 'B';
}

ZTEST(framework_core, test_evt_subscribe_order_and_capacity)
{
	/* 确定性：分发顺序 = 注册表静态顺序（LLD §4） */
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_STEP, sub_a, NULL), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_STEP, sub_b, NULL), TS_OK);

	order_n = 0;
	const ts_evt_t e = {.id = TS_EVT_BOOT_STEP, .t_ms = 0};
	ts_evt_publish(&e);
	zassert_equal(order_n, 2, "both subscribers called");
	zassert_equal(order_log[0], 'A', "static order A first");
	zassert_equal(order_log[1], 'B', "static order B second");

	/* 容量 CONFIG_TS_CORE_MAX_SUBS=4（DEC-27）：第 5 个订阅拒绝 */
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_STEP, sub_a, NULL), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_STEP, sub_b, NULL), TS_OK);
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_STEP, sub_a, NULL), TS_E_NOMEM);
	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_DONE, sub_a, NULL), TS_OK); /* 别的事件独立容量 */
}

static int isr_evt_seen;

static void isr_publish_fn(const void *arg)
{
	const ts_evt_t *e = arg;

	ts_evt_publish(e); /* [ISR] 路径：投递队列 */
}

static void evt_boot_done_cb(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	*((int *)u) += 1;
}

ZTEST(framework_core, test_evt_isr_queue)
{
	static int seen;

	zassert_equal(ts_evt_subscribe(TS_EVT_BOOT_DONE, evt_boot_done_cb, &seen), TS_OK);
	const ts_evt_t e = {.id = TS_EVT_BOOT_DONE, .t_ms = 42};

	seen = 0;
	isr_evt_seen = 0;
	irq_offload(isr_publish_fn, &e);
	zassert_equal(seen, 0, "ISR publish must be queued, not dispatched inline");
	ts_evt_poll_drain();
	zassert_equal(seen, 1, "queued event dispatched after drain");
}

static int warn_seen;
static ts_wdt_warn_evt_t last_warn;

static void wdt_warn_cb(const ts_evt_t *e, void *u)
{
	ARG_UNUSED(u);
	const ts_wdt_warn_evt_t *p = e->data;

	warn_seen++;
	last_warn = *p;
}

ZTEST(framework_core, test_wdt_math_and_failover)
{
	ts_time_test_bind(&virt_src);
	virt_now = 1000;

	zassert_equal(ts_wdt_register(TS_WDT_NET, 100), TS_OK);
	zassert_equal(ts_wdt_patrol_period_ms(), 50, "patrol = min/2 (LLD §5)");

	zassert_equal(ts_evt_subscribe(TS_EVT_WDT_WARN, wdt_warn_cb, NULL), TS_OK);

	/* DEC-48① 激活语义：注册未喂狗（未激活）的源不参与逾期判定 */
	virt_now = 1300;
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_COUNT,
		      "inactive source ignored (DEC-48 activation)");

	/* 首次 feed = 激活（时间戳 1300） */
	ts_wdt_feed(TS_WDT_NET);

	/* 未逾期：返回 TS_WDT_COUNT */
	virt_now = 1350;
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_COUNT, "no overdue at +50ms");

	/* 虚拟时钟推进 150ms → NET 逾期（period 100）→ 警告 + 逾期源 */
	virt_now = 1451;
	warn_seen = 0;
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_NET, "NET overdue at +151ms");
	zassert_equal(warn_seen, 1, "WDT_WARN published");
	zassert_equal(last_warn.src, TS_WDT_NET, "warn carries source");

	/* 巡检失败路径 → system_fail（TS_TEST 构建不 halts，置态可断言） */
	static const ts_out_ch_t ch = {
		.uid = "wdt_probe",
		.kind = TS_CH_GPIO,
		.poweron = {.b = false},
		.linkloss = {.b = false},
		.fault = {.b = true},
	};
	zassert_equal(ts_safety_register_channel(&ch), TS_OK);
	ts_safety_system_fail(TS_FAIL_WDT(TS_WDT_NET));
	zassert_equal(ts_safety_test_fail_reason(), TS_FAIL_WDT(TS_WDT_NET),
		      "fail reason observable");
	ts_ch_state_t st;
	zassert_equal(ts_safety_channel_state("wdt_probe", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_FAULT, "system fail → SAFE_FAULT");

	/* R1·DEC-64①：豁免窗数学——窗内不判逾/超窗恢复判逾/feed 清除/
	 * budget=0 no-op/60s 钳制/不激活未激活源 */
	ts_wdt_feed(TS_WDT_NET); /* 基线 1451 */
	virt_now = 1600;         /* elapsed=149 > period=100：无窗必逾 */
	ts_wdt_grace(TS_WDT_NET, 400);
	zassert_true(ts_wdt_grace_active(TS_WDT_NET), "grace window set");
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_COUNT, "窗内不判逾");
	virt_now = 2001; /* deadline=2000 已过 */
	zassert_false(ts_wdt_grace_active(TS_WDT_NET), "窗过期查询为假");
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_NET, "超窗恢复判逾（fail-safe 保持）");
	ts_wdt_feed(TS_WDT_NET); /* 基线 2001 + 清窗 */
	virt_now = 2150;
	ts_wdt_grace(TS_WDT_NET, 0);
	zassert_false(ts_wdt_grace_active(TS_WDT_NET), "budget=0 no-op");
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_NET, "zero-budget 不设窗：照常判逾");
	ts_wdt_feed(TS_WDT_NET); /* 基线 2150 */
	virt_now = 2151;
	ts_wdt_grace(TS_WDT_NET, 100000); /* 超 60s 上限 → 钳制生效 */
	zassert_true(ts_wdt_grace_active(TS_WDT_NET), "钳制后窗在位");
	virt_now = 62150; /* 钳制 deadline = 2151+60000=62151：窗内最后一点 */
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_COUNT, "钳制窗仍生效");
	virt_now = 62151; /* == deadline：窗闭（窗域 [now, deadline)）→ 判逾 */
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_NET, "钳制窗超时恢复判逾");
	ts_wdt_feed(TS_WDT_NET); /* 基线 62152 */
	virt_now = 63000;
	ts_wdt_deactivate(TS_WDT_NET);
	ts_wdt_grace(TS_WDT_NET, 400);
	virt_now = 63501; /* 若 grace 误激活源：deadline 63400 已超 → 逾期可见 */
	zassert_equal(ts_wdt_patrol_once(), TS_WDT_COUNT,
		      "grace 不激活未激活源（DEC-48① 语义不变）");
	ts_time_test_bind(NULL);
}

ZTEST(framework_core, test_boot_step_order_is_spec)
{
	/* 顺序是规格（HLD §4.4）：数组既有次序不可调换——此测试是守卫。
	 * M3a.2 net_init 尾部追加；M2b.2 app_load（步骤 8）尾部追加。 */
	static const char *const expected[] = {
		"estop_gpio", "safety_poweron", "wdt_start", "core_init",
#if defined(CONFIG_TS_NET)
		"net_init",
#endif
#if defined(CONFIG_TS_APP_WAMR)
		"app_load",
#endif
	};

	zassert_equal(TS_BOOT_STEP_COUNT,
#if defined(CONFIG_TS_NET) && defined(CONFIG_TS_APP_WAMR)
		      6,
#elif defined(CONFIG_TS_NET) || defined(CONFIG_TS_APP_WAMR)
		      5,
#else
		      4,
#endif
		      "step count = M1 四步 + 尾部追加（net/app_load）");
	for (int i = 0; i < TS_BOOT_STEP_COUNT; i++) {
		zassert_ok(strcmp(ts_boot_steps[i].name, expected[i]),
			   "boot step order changed at %d", i);
		zassert_not_null(ts_boot_steps[i].fn, "step fn wired");
	}
}

ZTEST_SUITE(framework_core, NULL, NULL, NULL, NULL, NULL);
