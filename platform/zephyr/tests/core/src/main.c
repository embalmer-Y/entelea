/* SPDX-License-Identifier: Apache-2.0 */
/* R02 监督机制 L1 测试（迁自旧 firmware/tests/core/src/main.c，oracle 语义
 * 保留；时间源改实例时钟绑定，旧 ts_time_ms 全局面移除）。 */
#include <zephyr/kernel.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>
#include <entelea/events.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"

static uint64_t virt_now;
static bool clock_fail_next; /* 一次性读失败注入（缺陷 5 负测） */

static entelea_result_t virt_read_ms(void *user, uint64_t *out)
{
	ARG_UNUSED(user);
	if (clock_fail_next) {
		clock_fail_next = false;
		return ENTELEA_E_IO;
	}
	*out = virt_now;
	return ENTELEA_OK;
}

static entelea_clock_t virt_clock = {
	.read_ms = virt_read_ms,
	.user = NULL,
	.clock_id = 0x5A5A,
	.last_ms = 0,
	.sampled = false,
};

/* ---- 虚拟时钟绑定与 WDT 数学（单用例顺序场景，套件内不依赖用例序） ---- */
static int order_log[8];
static int order_n;

static void sub_a(const entelea_event_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	order_log[order_n++] = 'A';
}

static void sub_b(const entelea_event_t *e, void *u)
{
	ARG_UNUSED(e);
	ARG_UNUSED(u);
	order_log[order_n++] = 'B';
}

ZTEST(framework_core, test_evt_subscribe_order_and_capacity)
{
	/* 确定性：分发顺序 = 注册表静态顺序。 */
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_a, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_b, NULL),
		      ENTELEA_OK);

	order_n = 0;
	const entelea_event_t e = {.id = ENTELEA_EVT_BOOT_STEP};

	entelea_evt_publish(&e);
	zassert_equal(order_n, 2, "both subscribers called");
	zassert_equal(order_log[0], 'A', "static order A first");
	zassert_equal(order_log[1], 'B', "static order B second");

	/* 容量 4（DEC-27）：第 5 个订阅拒绝。 */
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_a, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_b, NULL),
		      ENTELEA_OK);
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_STEP, sub_a, NULL),
		      ENTELEA_E_NOMEM);
	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_DONE, sub_a, NULL),
		      ENTELEA_OK, "别的事件独立容量");
}

static int isr_evt_seen;
/* ISR 投递的 data 必须指向静态存储（借用寿命）：载荷经队列延迟分发。 */
struct isr_payload_rec {
	uint32_t magic;
};
static const struct isr_payload_rec isr_payload = {.magic = 0xC0FFEEU};

static void isr_publish_fn(const void *arg)
{
	const entelea_event_t *e = arg;

	entelea_evt_publish(e); /* [ISR] 路径：投递队列 */
}

static void evt_boot_done_cb(const entelea_event_t *e, void *u)
{
	ARG_UNUSED(e);
	if (e->data != NULL && e->len == sizeof(isr_payload) &&
	    ((const struct isr_payload_rec *)e->data)->magic == 0xC0FFEEU) {
		*((int *)u) += 1;
	}
}

ZTEST(framework_core, test_evt_isr_queue_and_payload_lifetime)
{
	static int seen;

	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_BOOT_DONE, evt_boot_done_cb,
					    &seen),
		      ENTELEA_OK);
	const entelea_event_t e = {
		.id = ENTELEA_EVT_BOOT_DONE,
		.data = &isr_payload,
		.len = sizeof(isr_payload),
	};

	seen = 0;
	isr_evt_seen = 0;
	irq_offload(isr_publish_fn, &e);
	zassert_equal(seen, 0, "ISR publish must be queued, not dispatched inline");
	entelea_evt_poll_drain();
	zassert_equal(seen, 1, "queued event dispatched after drain");
	zassert_equal(entelea_evt_queue_dropped(), 0, "no drops expected");
}

static int warn_seen;
static entelea_wdt_warn_evt_t last_warn;

static void wdt_warn_cb(const entelea_event_t *e, void *u)
{
	ARG_UNUSED(u);
	const entelea_wdt_warn_evt_t *p = e->data;

	warn_seen++;
	last_warn = *p;
}

ZTEST(framework_core, test_wdt_math_and_failover)
{
	/* 用例序不保证：先净基线（源表/halted/护栏）。 */
	entelea_wdt_test_reset();
	/* 未绑定不伪造默认时基：register/start 显式拒绝（旧
	 * test_time_virtual_clock 的实例绑定面）。 */
	zassert_equal(entelea_wdt_bind_clock(NULL), ENTELEA_OK);
	zassert_equal(entelea_wdt_register(ENTELEA_WDT_SYWORK, 100),
		      ENTELEA_E_UNINIT, "unbound clock must refuse register");
	zassert_equal(entelea_wdt_start(), ENTELEA_E_UNINIT,
		      "unbound clock must refuse start");

	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 1000;
	zassert_equal(entelea_wdt_register(ENTELEA_WDT_SYWORK, 100), ENTELEA_OK);
	zassert_equal(entelea_wdt_register(ENTELEA_WDT_SYWORK, 100),
		      ENTELEA_E_STATE, "已注册源重复注册拒绝");
	zassert_equal(entelea_wdt_patrol_period_ms(), 50, "patrol = min/2");

	zassert_equal(entelea_evt_subscribe(ENTELEA_EVT_WDT_WARN, wdt_warn_cb, NULL),
		      ENTELEA_OK);

	/* 激活语义：注册未喂狗（未激活）的源不参与逾期判定。 */
	virt_now = 1300;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT,
		      "inactive source ignored");

	/* 首次 feed = 激活（时间戳 1300）。 */
	entelea_wdt_feed(ENTELEA_WDT_SYWORK);

	virt_now = 1350;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT,
		      "no overdue at +50ms");

	/* 虚拟时钟推进 150ms → 逾期（period 100）→ 警告 + 逾期源。 */
	virt_now = 1451;
	warn_seen = 0;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_SYWORK,
		      "overdue at +151ms");
	zassert_equal(warn_seen, 1, "WDT_WARN published");
	zassert_equal(last_warn.src, ENTELEA_WDT_SYWORK, "warn carries source");

	/* 巡检失败路径 → system_fail（测试构建不 halt，置态可断言）。 */
	static const entelea_out_channel_t ch = {
		.uid = "wdt_probe",
		.kind = ENTELEA_CH_GPIO,
		.poweron = {.b = false},
		.linkloss = {.b = false},
		.fault = {.b = true},
	};
	zassert_equal(entelea_safety_register_channel(&ch), ENTELEA_OK);
	entelea_safety_system_fail(ENTELEA_FAIL_WDT(ENTELEA_WDT_SYWORK));
	zassert_equal(entelea_safety_test_fail_reason(),
		      ENTELEA_FAIL_WDT(ENTELEA_WDT_SYWORK), "fail reason observable");
	entelea_ch_state_t st;

	zassert_equal(entelea_safety_channel_state("wdt_probe", &st), ENTELEA_OK);
	zassert_equal(st, ENTELEA_ST_SAFE_FAULT, "system fail -> SAFE_FAULT");

	/* 豁免窗数学：窗内不判逾/超窗恢复判逾/feed 清除/budget=0 no-op/
	 * 60s 钳制/不激活未激活源。 */
	entelea_wdt_feed(ENTELEA_WDT_SYWORK); /* 基线 1451 */
	virt_now = 1600; /* elapsed=149 > period=100：无窗必逾 */
	entelea_wdt_grace(ENTELEA_WDT_SYWORK, 400);
	zassert_true(entelea_wdt_grace_active(ENTELEA_WDT_SYWORK), "grace set");
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT, "窗内不判逾");
	virt_now = 2001; /* deadline=2000 已过 */
	zassert_false(entelea_wdt_grace_active(ENTELEA_WDT_SYWORK), "窗过期为假");
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_SYWORK,
		      "超窗恢复判逾（fail-safe 保持）");
	entelea_wdt_feed(ENTELEA_WDT_SYWORK); /* 基线 2001 + 清窗 */
	virt_now = 2150;
	entelea_wdt_grace(ENTELEA_WDT_SYWORK, 0);
	zassert_false(entelea_wdt_grace_active(ENTELEA_WDT_SYWORK), "budget=0 no-op");
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_SYWORK,
		      "zero-budget 不设窗：照常判逾");
	entelea_wdt_feed(ENTELEA_WDT_SYWORK); /* 基线 2150 */
	virt_now = 2151;
	entelea_wdt_grace(ENTELEA_WDT_SYWORK, 100000); /* 超 60s 上限 → 钳制 */
	zassert_true(entelea_wdt_grace_active(ENTELEA_WDT_SYWORK), "钳制后窗在位");
	virt_now = 62150; /* 钳制 deadline = 2151+60000=62151：窗内最后一点 */
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT, "钳制窗仍生效");
	virt_now = 62151; /* == deadline：窗闭 → 判逾 */
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_SYWORK,
		      "钳制窗超时恢复判逾");
	entelea_wdt_feed(ENTELEA_WDT_SYWORK); /* 基线 62152 */
	virt_now = 63000;
	entelea_wdt_deactivate(ENTELEA_WDT_SYWORK);
	entelea_wdt_grace(ENTELEA_WDT_SYWORK, 400);
	virt_now = 63501; /* 若 grace 误激活源：deadline 63400 已超 → 逾期可见 */
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT,
		      "grace 不激活未激活源");
}

/* ---- 时钟单调护栏（缺陷 5）：读失败/回退沿用最后有效值，不伪 0 ---- */
ZTEST(framework_core, test_clock_guard_keeps_last_good)
{
	entelea_wdt_test_reset();
	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 5000;
	zassert_equal(entelea_wdt_register(ENTELEA_WDT_SYWORK, 1000), ENTELEA_OK);
	entelea_wdt_feed(ENTELEA_WDT_SYWORK); /* last_feed = last_good = 5000 */
	zassert_equal(entelea_wdt_clock_faults(), 0);
	/* 读失败（时间仅前进到界内 5200）：护栏沿用 5000 → elapsed=0。
	 * 修复前：失败→0 → elapsed=0-5000 下溢 → 伪逾期。 */
	virt_now = 5200;
	clock_fail_next = true;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT,
		      "clock read failure must not fabricate overdue");
	zassert_true(entelea_wdt_clock_faults() >= 1, "failure counted");
	/* 回退（不 feed）：last_feed=5200，时钟读回 100。护栏沿用 5200 →
	 * elapsed=0；修复前 100-5200 下溢 → 伪逾期。 */
	virt_now = 5200;
	entelea_wdt_feed(ENTELEA_WDT_SYWORK);
	virt_now = 100;
	zassert_true(entelea_wdt_patrol_once() == ENTELEA_WDT_COUNT,
		     "regressed clock must not fabricate overdue");
	zassert_true(entelea_wdt_clock_faults() >= 2, "regression counted");
	entelea_wdt_test_reset();
}

/* ---- 时钟失明策略（A1）：连续 ≥3 轮巡检时间不可知 → fail-safe ---- */
ZTEST(framework_core, test_wdt_blind_clock_fails_safe)
{
	entelea_wdt_test_reset();
	entelea_safety_test_reset();
	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 5000;
	zassert_equal(entelea_wdt_register(ENTELEA_WDT_SYWORK, 1000), ENTELEA_OK);
	entelea_wdt_feed(ENTELEA_WDT_SYWORK);

	/* 单轮/两轮失败：不判逾、不假活、不触发 fail-safe；好读复位轮数。 */
	for (int round = 0; round < 2; round++) {
		clock_fail_next = true;
		zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT);
	}
	zassert_equal(entelea_safety_test_fail_reason(), 0, "两轮失明不得提前 fail");
	virt_now = 5200;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT, "好读恢复判时");
	clock_fail_next = true;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT);
	/* 复位后重新累计：1 轮失明不触发。 */
	virt_now = 5300;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT);

	/* 连续第 3 轮失明 → system_fail（reason 载荷 0xFF=时钟失明）+ 全局停机。 */
	for (int round = 0; round < 2; round++) {
		clock_fail_next = true;
		zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT);
	}
	clock_fail_next = true;
	(void)entelea_wdt_patrol_once();
	zassert_equal(entelea_safety_test_fail_reason(),
		      ENTELEA_FAIL_WDT_CLOCK_BLIND, "3 轮失明必须 fail-safe");
	zassert_false(entelea_wdt_test_patrol_tick(), "fail 后巡检不得执行");
	entelea_wdt_test_reset();
	entelea_safety_test_reset();
}

/* ---- 持续回退同计失明（计划符合性审查 P2）：真回退不得把轮数归零 ---- */
ZTEST(framework_core, test_wdt_persistent_rollback_counts_blind)
{
	entelea_wdt_test_reset();
	entelea_safety_test_reset();
	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 5000;
	zassert_equal(entelea_wdt_register(ENTELEA_WDT_SYWORK, 1000), ENTELEA_OK);
	entelea_wdt_feed(ENTELEA_WDT_SYWORK);

	/* 两轮回退：不判逾不假活（修复前回退 ok=true 把轮数归零，
	 * 第 3 轮也永不 fail——持续回退后端下 A1 静默失效）。 */
	virt_now = 4000;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT,
		      "回退轮不得伪逾期");
	virt_now = 3000;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT);
	zassert_equal(entelea_safety_test_fail_reason(), 0, "两轮回退不得提前 fail");
	/* 好读复位轮数（回退后恢复前进 = 时间重新可知）。 */
	virt_now = 5100;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT, "好读恢复判时");
	/* 重新累计：3 轮连续回退 → fail-safe（载荷 0xFF）。 */
	virt_now = 4900;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT);
	virt_now = 4800;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_COUNT);
	virt_now = 4700;
	(void)entelea_wdt_patrol_once();
	zassert_equal(entelea_safety_test_fail_reason(),
		      ENTELEA_FAIL_WDT_CLOCK_BLIND,
		      "持续回退必须按 A1 计入失明轮数并 fail-safe");
	zassert_false(entelea_wdt_test_patrol_tick());
	entelea_wdt_test_reset();
	entelea_safety_test_reset();
}

/* ---- ISR 上下文并发冒烟（R-E）：feed 从 ISR 调用，簿记一致 ---- */
static void isr_feed_fn(const void *arg)
{
	ARG_UNUSED(arg);
	entelea_wdt_feed(ENTELEA_WDT_SYWORK);
}

ZTEST(framework_core, test_isr_context_clock_bookkeeping)
{
	entelea_wdt_test_reset();
	zassert_equal(entelea_wdt_bind_clock(&virt_clock), ENTELEA_OK);
	virt_now = 100;
	zassert_equal(entelea_wdt_register(ENTELEA_WDT_SYWORK, 1000), ENTELEA_OK);
	for (int i = 0; i < 30; i++) {
		virt_now = 100 + (i + 1) * 100;
		irq_offload(isr_feed_fn, NULL); /* ISR 上下文读时钟（临界区内簿记） */
	}
	zassert_equal(entelea_wdt_clock_faults(), 0, "无撕裂/失败计数");
	/* 末次 feed @3100；+1001 → 精确逾期（证明 ISR 侧时间戳未丢失）。 */
	virt_now = 4101;
	zassert_equal(entelea_wdt_patrol_once(), ENTELEA_WDT_SYWORK,
		      "ISR-side feeds must be honored precisely");
	entelea_wdt_test_reset();
}

ZTEST(framework_core, test_boot_step_order_is_spec)
{
	/* 顺序是规格：既有次序不可调换——此测试是守卫。
	 * R02 四步 = 旧 M1 四步（core_init 占位更名 supervision_ready）；
	 * 后续业务批次只在尾部追加。 */
	static const char *const expected[] = {
		"estop_gpio", "safety_poweron", "wdt_start", "supervision_ready",
		"storage_init", "d12_bootguard",
	};

	zassert_equal(ENTELEA_PLATFORM_BOOT_STEP_COUNT, 6,
		      "step count = r16 d12 首步 + R02 四步 + R03 尾部追加");
	for (int i = 0; i < ENTELEA_PLATFORM_BOOT_STEP_COUNT; i++) {
		zassert_ok(strcmp(entelea_platform_boot_steps[i].name, expected[i]),
			   "boot step order changed at %d", i);
		zassert_not_null(entelea_platform_boot_steps[i].fn, "step fn wired");
	}
}

ZTEST_SUITE(framework_core, NULL, NULL, NULL, NULL, NULL);
