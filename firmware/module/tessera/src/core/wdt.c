/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 看门狗框架（LLD-ts-core §5；DEC-48 接线批）：喂狗注册表 + sysworkq 巡检。
 * 合同 4：每子系统独立喂狗（可定位卡死来源）；逾期 → WDT_WARN → system_fail。
 * DEC-48 四层防线落点：
 *   L1 本软看门狗三源（静态预注册 + 动态激活——注册窗口与启动序解耦：
 *       boot 步骤 3 启动巡检，子系统首次 feed 激活各自源）；
 *   L3 Zephyr task_wdt 通道（callback=NULL → 过期即 sys_reboot——软巡检
 *       自身卡死兜底；硬件回退经 CONFIG_TASK_WDT_HW_FALLBACK + 板级
 *       chosen zephyr,watchdog——kernel 死透时 MWDT 最后兜底）；
 *   L4 system_fail noinit 留痕（强实现 store/noinit.c）+ 停机后通道无人
 *       喂 → sys_reboot（k_sleep 不停 k_timer）。注：task_wdt 直通复位路径
 *       无 noinit 留痕（ISR 上下文不可写 flash）——该极端场景归因缺失，
 *       如实在 docs/history/review-archive（impl-review-02 节）/DEC-48 登记；常规逾期经巡检 → system_fail
 *       → 有留痕。
 */
#include <ts/core.h>
#include <ts/safety.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#ifdef CONFIG_TASK_WDT
#include <zephyr/device.h>
#include <zephyr/task_wdt/task_wdt.h>
#endif
#if defined(CONFIG_TS_STORE)
#include <ts/store.h>
#endif

struct wdt_src {
	uint32_t period_ms;
	atomic_t last_feed32; /* ts_time_ms 低 32 位；无符号差值对回绕安全 */
	atomic_t grace_until32; /* R1·DEC-64①：豁免窗绝对截止（0 = 无；feed 清除） */
	bool registered;
	bool active; /* DEC-48①：首次 feed 激活——未激活源不参与逾期判定 */
};

static struct wdt_src srcs[TS_WDT_COUNT];
static bool started;

/* 硬 WDT 超时 = min(2×max(periods), 10000ms)（来源: DEC-22 / DEC-27） */
#define TS_HARD_WDT_CAP_MS 10000

/* 豁免窗预算钳制（R1·DEC-64①）：防误用长预算把失防窗扩到分钟级 */
#define TS_WDT_GRACE_CAP_MS 60000

#ifdef CONFIG_TASK_WDT
static int twdt_ch = -1;
#endif

static uint32_t elapsed_since(const struct wdt_src *s)
{
	uint32_t now32 = (uint32_t)ts_time_ms();
	uint32_t last = (uint32_t)atomic_get(&s->last_feed32);

	return now32 - last;
}

ts_res_t ts_wdt_register(ts_wdt_src_t src, uint32_t period_ms)
{
	if (src >= TS_WDT_COUNT || period_ms == 0 || started) {
		return TS_E_PARAM;
	}
	if (srcs[src].registered) {
		return TS_E_STATE;
	}
	srcs[src].registered = true;
	srcs[src].period_ms = period_ms;
	atomic_set(&srcs[src].last_feed32, (atomic_val_t)(uint32_t)ts_time_ms());
	return TS_OK;
}

void ts_wdt_feed(ts_wdt_src_t src)
{
	if (src < TS_WDT_COUNT) {
		atomic_set(&srcs[src].last_feed32, (atomic_val_t)(uint32_t)ts_time_ms());
		atomic_set(&srcs[src].grace_until32, 0); /* R1：正常喂狗恢复即清豁免窗 */
		srcs[src].active = true; /* DEC-48①：首次喂狗 = 激活 */
	}
}

void ts_wdt_grace(ts_wdt_src_t src, uint32_t budget_ms)
{
	if (src >= TS_WDT_COUNT || budget_ms == 0) {
		return;
	}
	struct wdt_src *s = &srcs[src];

	/* 豁免不激活源（激活 = feed 专属，DEC-48① 语义不变） */
	if (!s->registered || !s->active) {
		return;
	}
	if (budget_ms > TS_WDT_GRACE_CAP_MS) {
		budget_ms = TS_WDT_GRACE_CAP_MS;
	}
	/* 绝对截止 = now + budget；u32 回绕由签名差值比较吸收 */
	atomic_set(&s->grace_until32,
		   (atomic_val_t)((uint32_t)ts_time_ms() + budget_ms));
}

void ts_wdt_deactivate(ts_wdt_src_t src)
{
	/* 单元 F 补全（DEC-48① 动态激活的对称面）：APPMGR 源随 APP 停止去激活
	 * ——stop 后无人喂是预期态，不得判逾期（真机 inputdemo 实证：stop 后
	 * 1000ms 逾期 → system_fail 复位，D6 永不上场）。下次 feed 重新激活。 */
	if (src < TS_WDT_COUNT) {
		srcs[src].active = false;
	}
}

/* 巡检：返回首个逾期源；无则 TS_WDT_COUNT。 */
ts_wdt_src_t ts_wdt_patrol_once(void)
{
	uint32_t now32 = (uint32_t)ts_time_ms();

	for (int s = 0; s < TS_WDT_COUNT; s++) {
		if (!srcs[s].registered || !srcs[s].active) {
			continue;
		}
		if (elapsed_since(&srcs[s]) > srcs[s].period_ms) {
			/* R1·DEC-64①：豁免窗内不判逾（声明式合法阻塞期）；
			 * 超窗恢复判逾——预算过期 = 真卡死，fail-safe 保持 */
			uint32_t gu = (uint32_t)atomic_get(&srcs[s].grace_until32);

			if (gu != 0 && (int32_t)(now32 - gu) < 0) {
				continue;
			}
			/* LLD §5：先 WDT_WARN（带 src 与最后 feed 时间）→ system_fail */
			struct {
				ts_wdt_src_t src;
				uint32_t last_feed;
			} payload = {
				.src = (ts_wdt_src_t)s,
				.last_feed = (uint32_t)atomic_get(&srcs[s].last_feed32),
			};
			const ts_evt_t evt = {
				.id = TS_EVT_WDT_WARN,
				.t_ms = ts_time_ms(),
				.data = &payload,
				.len = sizeof(payload),
			};

			ts_evt_publish(&evt);
			return (ts_wdt_src_t)s;
		}
	}
	return TS_WDT_COUNT;
}

static void patrol_work_cb(struct k_work *work)
{
	struct k_work_delayable *d = k_work_delayable_from_work(work);

	ts_wdt_feed(TS_WDT_SYWORK); /* DEC-48②：巡检自身运行 = sysworkq 活性证明 */
#ifdef CONFIG_TASK_WDT
	if (twdt_ch >= 0) {
		task_wdt_feed(twdt_ch); /* L3：软巡检卡死 → 通道过期 → sys_reboot */
	}
#endif
	ts_safety_estop_deferred_publish(); /* estop 事后补发（合同 5，复用巡检节奏） */
	ts_wdt_src_t overdue = ts_wdt_patrol_once();

	if (overdue != TS_WDT_COUNT) {
		ts_safety_system_fail(TS_FAIL_WDT(overdue));
		/* 停止重排巡检（停喂语义由 system_fail 停机保证；DEC-48④：停机后
		 * task_wdt 通道无人喂 → sys_reboot 复位闭环） */
		return;
	}
	uint32_t delay = ts_wdt_patrol_period_ms();

	k_work_reschedule_for_queue(&k_sys_work_q, d, K_MSEC(delay));
}

static struct k_work_delayable patrol_work;

uint32_t ts_wdt_patrol_period_ms(void)
{
	uint32_t min_p = UINT32_MAX;

	for (int s = 0; s < TS_WDT_COUNT; s++) {
		if (srcs[s].registered && srcs[s].period_ms < min_p) {
			min_p = srcs[s].period_ms;
		}
	}
	if (min_p == UINT32_MAX) {
		min_p = TS_HARD_WDT_CAP_MS; /* 无注册源：按硬 WDT 上限巡检 */
	}
	return min_p / 2 < 1 ? 1 : min_p / 2; /* 巡检周期 = min(periods)/2（LLD §5） */
}

ts_res_t ts_wdt_start(void)
{
	if (started) {
		return TS_E_STATE;
	}
	/* DEC-48①：三源静态预注册（Kconfig 周期，Q-28② 裁定；注册表此后不可变
	 * ——零并发面）。激活 = 各子系统首次 ts_wdt_feed。 */
	srcs[TS_WDT_NET].registered = true;
	srcs[TS_WDT_NET].period_ms = CONFIG_TS_WDT_NET_PERIOD_MS;
	atomic_set(&srcs[TS_WDT_NET].last_feed32, (atomic_val_t)(uint32_t)ts_time_ms());
	srcs[TS_WDT_APPMGR].registered = true;
	srcs[TS_WDT_APPMGR].period_ms = CONFIG_TS_WDT_APPMGR_PERIOD_MS;
	atomic_set(&srcs[TS_WDT_APPMGR].last_feed32, (atomic_val_t)(uint32_t)ts_time_ms());
	srcs[TS_WDT_SYWORK].registered = true;
	srcs[TS_WDT_SYWORK].period_ms = CONFIG_TS_WDT_SYWORK_PERIOD_MS;
	atomic_set(&srcs[TS_WDT_SYWORK].last_feed32, (atomic_val_t)(uint32_t)ts_time_ms());

#ifdef CONFIG_TASK_WDT
	const struct device *hw = NULL;

#if defined(CONFIG_TASK_WDT_HW_FALLBACK) && DT_HAS_CHOSEN(zephyr_watchdog)
	hw = DEVICE_DT_GET(DT_CHOSEN(zephyr_watchdog));
#endif
	task_wdt_init(hw);
	/* callback=NULL → 通道过期 = sys_reboot(COLD)（task_wdt.c 语义） */
	twdt_ch = task_wdt_add(CONFIG_TS_WDT_CHANNEL_MS, NULL, NULL);
#endif

#if defined(CONFIG_TS_STORE)
	{ /* L4 观测：上次异常复位留痕读出（system_fail 路径写入；task_wdt
	   * 直通复位无留痕——见文件头注）。 */
		uint8_t rec[8];
		uint16_t len = sizeof(rec);
		bool fresh = true;

		if (ts_store_noinit_get(rec, &len, &fresh) == TS_OK && !fresh &&
		    len >= sizeof(uint32_t)) {
			uint32_t reason = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) |
					  ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 24);

			printk("[wdt] 上次异常复位留痕 reason=0x%08x（noinit 归因）\n",
			       reason);
		}
	}
#endif
	started = true;
	k_work_init_delayable(&patrol_work, patrol_work_cb);
	k_work_reschedule_for_queue(&k_sys_work_q, &patrol_work,
				    K_MSEC(ts_wdt_patrol_period_ms()));
	return TS_OK;
}

#ifdef CONFIG_TS_TEST
bool ts_wdt_grace_active(ts_wdt_src_t src)
{
	if (src >= TS_WDT_COUNT) {
		return false;
	}
	uint32_t gu = (uint32_t)atomic_get(&srcs[src].grace_until32);

	return gu != 0 && (int32_t)((uint32_t)ts_time_ms() - gu) < 0;
}
#endif
