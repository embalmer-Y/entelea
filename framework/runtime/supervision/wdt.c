/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 软看门狗框架（迁自旧 core/wdt.c，R02 合同 framework/runtime/supervision/contract.md）。
 * 合同 4：每子系统独立喂狗（可定位卡死来源）；逾期 → WDT_WARN → system_fail。
 * 分层防线落点（DEC-48）：L1 本软看门狗（静态预注册 + 动态激活；boot 步骤
 * 启动巡检，子系统首次 feed 激活各自源）；L3 Zephyr task_wdt 通道
 * （CONFIG_TASK_WDT；callback=NULL → 过期即 sys_reboot——软巡检自身卡死兜底；
 * 硬件回退经 CONFIG_TASK_WDT_HW_FALLBACK + 板级 chosen zephyr,watchdog）。
 * 相对旧实现调整：时间源改实例时钟绑定（feed 的 ISR 安全性见合同）；
 * 源集合 R02 仅 SYWORK（NET/APPMGR 随其子系统批次加入）；
 * noinit 留痕/上次复位读出归 R03。
 */
#include <errno.h>
#include <string.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#ifdef CONFIG_TASK_WDT
#include <zephyr/device.h>
#include <zephyr/task_wdt/task_wdt.h>
#endif

struct wdt_src {
	uint32_t period_ms;
	atomic_t last_feed32; /* 时钟低 32 位；无符号差值对回绕安全 */
	atomic_t grace_until32; /* 豁免窗绝对截止（0 = 无；feed 清除） */
	bool registered;
	bool active; /* 首次 feed 激活——未激活源不参与逾期判定 */
};

static struct wdt_src srcs[ENTELEA_WDT_COUNT];
static bool started;
static const entelea_clock_t *wdt_clock;
static atomic_t halted = ATOMIC_INIT(0); /* 全局停机（system_fail 置位） */
static uint64_t clock_last_good;
static uint32_t clock_fault_count;

#ifdef CONFIG_TASK_WDT
static int twdt_ch = -1;
static bool twdt_initialized;
#endif

entelea_result_t entelea_wdt_bind_clock(const entelea_clock_t *clock)
{
	if (clock == NULL || clock->read_ms == NULL || clock->clock_id == 0) {
		/* 绑定 NULL 恢复未绑定（测试隔离用）。 */
		if (clock == NULL) {
			wdt_clock = NULL;
			return ENTELEA_OK;
		}
		return ENTELEA_E_PARAM;
	}
	wdt_clock = clock;
	return ENTELEA_OK;
}

/* 经绑定实例时钟的原始 read（feed 须可在 ISR 调用：绕过 clock_now 的
 * 非原子回退簿记；ISR 安全性要求随绑定声明，见 wdt.h）。
 * 单调护栏（缺陷 5）：读失败或回退沿用最后有效值并计数——失败伪造 0
 * 会使 elapsed 突变触发伪逾期（fail 误动作）。 */
static uint32_t now32(void)
{
	uint64_t ms = 0;

	if (wdt_clock == NULL) {
		return (uint32_t)clock_last_good;
	}
	if (wdt_clock->read_ms(wdt_clock->user, &ms) != ENTELEA_OK ||
	    ms < clock_last_good) {
		clock_fault_count++;
		return (uint32_t)clock_last_good;
	}
	clock_last_good = ms;
	return (uint32_t)ms;
}

uint32_t entelea_wdt_clock_faults(void)
{
	return clock_fault_count;
}

void entelea_wdt_halt(void)
{
	/* 全局停机（缺陷 4）：置位后巡检不喂、不重排、不判逾——任何线程
	 * 触发 system_fail 的停机语义相同，不依赖调用线程自身挂起；
	 * 停喂后 task_wdt 通道到期 → sys_reboot 兜底。 */
	atomic_set(&halted, 1);
}

static uint32_t elapsed_since(const struct wdt_src *s)
{
	return now32() - (uint32_t)atomic_get(&s->last_feed32);
}

entelea_result_t entelea_wdt_register(entelea_wdt_src_t src, uint32_t period_ms)
{
	if ((int)src < 0 || src >= ENTELEA_WDT_COUNT || period_ms == 0 || started) {
		return ENTELEA_E_PARAM;
	}
	if (wdt_clock == NULL) {
		return ENTELEA_E_UNINIT; /* 不伪造默认时基 */
	}
	if (srcs[src].registered) {
		return ENTELEA_E_STATE;
	}
	srcs[src].registered = true;
	srcs[src].period_ms = period_ms;
	atomic_set(&srcs[src].last_feed32, (atomic_val_t)now32());
	return ENTELEA_OK;
}

void entelea_wdt_feed(entelea_wdt_src_t src)
{
	if ((int)src >= 0 && src < ENTELEA_WDT_COUNT) {
		atomic_set(&srcs[src].last_feed32, (atomic_val_t)now32());
		/* 正常喂狗恢复即清豁免窗。 */
		atomic_set(&srcs[src].grace_until32, 0);
		srcs[src].active = true; /* 首次喂狗 = 激活 */
	}
}

void entelea_wdt_grace(entelea_wdt_src_t src, uint32_t budget_ms)
{
	if ((int)src < 0 || src >= ENTELEA_WDT_COUNT || budget_ms == 0) {
		return;
	}
	struct wdt_src *s = &srcs[src];

	/* 豁免不激活源（激活 = feed 专属语义不变）。 */
	if (!s->registered || !s->active) {
		return;
	}
	if (budget_ms > ENTELEA_WDT_GRACE_CAP_MS) {
		budget_ms = ENTELEA_WDT_GRACE_CAP_MS;
	}
	/* 绝对截止 = now + budget；u32 回绕由签名差值比较吸收。 */
	atomic_set(&s->grace_until32, (atomic_val_t)(now32() + budget_ms));
}

void entelea_wdt_deactivate(entelea_wdt_src_t src)
{
	/* 去激活对称面：源随其子系统停止，停后无人喂是预期态，不判逾期；
	 * 下次 feed 重新激活。 */
	if ((int)src >= 0 && src < ENTELEA_WDT_COUNT) {
		srcs[src].active = false;
	}
}

/* 巡检：返回首个逾期源；无则 ENTELEA_WDT_COUNT。 */
static entelea_wdt_src_t patrol_once_impl(void)
{
	uint32_t now = now32();

	for (int s = 0; s < ENTELEA_WDT_COUNT; s++) {
		if (!srcs[s].registered || !srcs[s].active) {
			continue;
		}
		if (elapsed_since(&srcs[s]) > srcs[s].period_ms) {
			/* 豁免窗内不判逾（声明式合法阻塞期）；超窗恢复判逾
			 * （预算过期 = 真卡死，fail-safe 保持）。 */
			uint32_t gu = (uint32_t)atomic_get(&srcs[s].grace_until32);

			if (gu != 0 && (int32_t)(now - gu) < 0) {
				continue;
			}
			/* 先 WDT_WARN（带 src 与最后 feed 时间）再进 system_fail。 */
			entelea_wdt_warn_evt_t payload = {
				.src = (int32_t)s,
				.last_feed = (uint32_t)atomic_get(&srcs[s].last_feed32),
			};
			const entelea_event_t evt = {
				.id = ENTELEA_EVT_WDT_WARN,
				.data = &payload,
				.len = sizeof(payload),
			};

			entelea_evt_publish(&evt);
			return (entelea_wdt_src_t)s;
		}
	}
	return ENTELEA_WDT_COUNT;
}

static void patrol_work_cb(struct k_work *work)
{
	struct k_work_delayable *d = k_work_delayable_from_work(work);

	if (atomic_get(&halted) != 0) {
		return; /* 全局停机：不喂、不重排（停喂语义由 halted 保证） */
	}
	entelea_wdt_feed(ENTELEA_WDT_SYWORK); /* 巡检自身运行 = sysworkq 活性证明 */
#ifdef CONFIG_TASK_WDT
	if (twdt_ch >= 0) {
		task_wdt_feed(twdt_ch); /* L3：软巡检卡死 → 通道过期 → sys_reboot */
	}
#endif
	/* estop 事后补发（复用巡检节奏）。 */
	entelea_safety_estop_deferred_publish();
	entelea_wdt_src_t overdue = patrol_once_impl();

	if (overdue != ENTELEA_WDT_COUNT) {
		entelea_safety_system_fail(ENTELEA_FAIL_WDT((uint32_t)overdue));
		/* system_fail 置 halted（先于停机）→ 巡检此后不再喂/重排；
		 * 停机后 task_wdt 通道无人喂 → sys_reboot 复位闭环。 */
		return;
	}
	uint32_t delay = entelea_wdt_patrol_period_ms();

	k_work_reschedule_for_queue(&k_sys_work_q, d, K_MSEC(delay));
}

static struct k_work_delayable patrol_work;

uint32_t entelea_wdt_patrol_period_ms(void)
{
	uint32_t min_p = UINT32_MAX;

	for (int s = 0; s < ENTELEA_WDT_COUNT; s++) {
		if (srcs[s].registered && srcs[s].period_ms < min_p) {
			min_p = srcs[s].period_ms;
		}
	}
	if (min_p == UINT32_MAX) {
		min_p = ENTELEA_WDT_HARD_CAP_MS; /* 无注册源：按硬 WDT 上限巡检 */
	}
	return min_p / 2 < 1 ? 1 : min_p / 2; /* 巡检周期 = min(periods)/2 */
}

entelea_result_t entelea_wdt_start(void)
{
	if (started || atomic_get(&halted) != 0) {
		return ENTELEA_E_STATE;
	}
	if (wdt_clock == NULL) {
		return ENTELEA_E_UNINIT; /* 不伪造默认时基 */
	}
	/* 静态预注册（周期常量出处 DEC-48；注册表此后不可变，零并发面）。
	 * 激活 = 各子系统首次 feed。 */
	srcs[ENTELEA_WDT_SYWORK].registered = true;
	srcs[ENTELEA_WDT_SYWORK].period_ms = ENTELEA_WDT_SYWORK_PERIOD_MS;
	atomic_set(&srcs[ENTELEA_WDT_SYWORK].last_feed32, (atomic_val_t)now32());

#ifdef CONFIG_TASK_WDT
	const struct device *hw = NULL;
	int ret;

#if defined(ENTELEA_WDT_REQUIRE_HW) && !DT_HAS_CHOSEN(zephyr_watchdog)
	return ENTELEA_E_IO; /* 必需硬件看门狗未选定：显式失败，不静默软兜底 */
#endif
#if defined(CONFIG_TASK_WDT_HW_FALLBACK) && DT_HAS_CHOSEN(zephyr_watchdog)
	hw = DEVICE_DT_GET(DT_CHOSEN(zephyr_watchdog));
	if (!device_is_ready(hw)) {
		return ENTELEA_E_IO;
	}
#endif
	if (!twdt_initialized) {
		ret = task_wdt_init(hw);
		if (ret != 0) {
			return ENTELEA_E_IO;
		}
		twdt_initialized = true;
	}
	/* callback=NULL → 通道过期 = sys_reboot(COLD)（task_wdt 语义）。 */
	twdt_ch = task_wdt_add(ENTELEA_WDT_CHANNEL_MS, NULL, NULL);
	if (twdt_ch < 0) {
		return twdt_ch == -ENOMEM ? ENTELEA_E_NOMEM : ENTELEA_E_IO;
	}
#endif

	started = true;
	k_work_init_delayable(&patrol_work, patrol_work_cb);
	k_work_reschedule_for_queue(&k_sys_work_q, &patrol_work,
				    K_MSEC(entelea_wdt_patrol_period_ms()));
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void entelea_wdt_test_reset(void)
{
	/* 测试隔离：先同步取消可能仍在调度的巡检项（对已调度 delayable
	 * 重复 init 是未定义行为），再清源表/启动/halted/时钟护栏。 */
	struct k_work_sync sync;

	(void)k_work_cancel_delayable_sync(&patrol_work, &sync);
	memset(srcs, 0, sizeof(srcs));
	started = false;
	atomic_set(&halted, 0);
	clock_last_good = 0;
	clock_fault_count = 0;
}

entelea_wdt_src_t entelea_wdt_patrol_once(void)
{
	return patrol_once_impl();
}

bool entelea_wdt_test_patrol_tick(void)
{
	/* 确定性巡检体驱动（ halted 测试用）：返回是否真正执行。 */
	if (atomic_get(&halted) != 0) {
		return false;
	}
	patrol_work_cb(&patrol_work.work);
	return true;
}

bool entelea_wdt_grace_active(entelea_wdt_src_t src)
{
	if ((int)src < 0 || src >= ENTELEA_WDT_COUNT) {
		return false;
	}
	uint32_t gu = (uint32_t)atomic_get(&srcs[src].grace_until32);

	return gu != 0 && (int32_t)(now32() - gu) < 0;
}
#endif
