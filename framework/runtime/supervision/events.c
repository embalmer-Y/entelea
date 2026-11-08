/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 事件总线（迁自旧 core/events.c，R02 合同 framework/runtime/supervision/contract.md）：
 * 静态注册表 + ISR 投递队列（sysworkq 分发）。确定性：分发顺序 = 注册表
 * 静态顺序（禁运行期插队）。相对旧实现调整：时间戳来源由全局 ts_time_ms
 * 改为实例时钟绑定；容量常量出处不变（DEC-27）。
 */
#include <entelea/events.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct sub_slot {
	entelea_event_cb_t fn;
	void *user;
};

static struct sub_slot subs[ENTELEA_EVT_ID_COUNT][ENTELEA_EVT_MAX_SUBS];

struct queued_evt {
	entelea_event_t evt;
};
K_MSGQ_DEFINE(evt_q, sizeof(struct queued_evt), ENTELEA_EVT_QUEUE_DEPTH, 8);

static atomic_t dropped_count;
static const entelea_clock_t *evt_clock;

entelea_result_t entelea_evt_bind_clock(const entelea_clock_t *clock)
{
	evt_clock = clock;
	return ENTELEA_OK;
}

/* 发布时间戳：经绑定时钟的原始 read（绕过 clock_now 的回退簿记——
 * 簿记非原子，ISR 投递与线程分发并发访问同一实例会撕裂 last_ms）。
 * 单调护栏（缺陷 5）：读失败/回退沿用最后有效值，不伪造 0；未绑定或
 * 从未成功 → 0（可观测的未配置）。 */
static uint64_t now_ms(void)
{
	static uint64_t last_good;
	uint64_t ms = 0;

	if (evt_clock == NULL || evt_clock->read_ms == NULL) {
		return last_good;
	}
	if (evt_clock->read_ms(evt_clock->user, &ms) != ENTELEA_OK || ms < last_good) {
		return last_good;
	}
	last_good = ms;
	return ms;
}

static void dispatch(const entelea_event_t *evt)
{
	for (int i = 0; i < ENTELEA_EVT_MAX_SUBS; i++) {
		const struct sub_slot *s = &subs[evt->id][i];

		if (s->fn != NULL) {
			s->fn(evt, s->user);
		}
	}
}

entelea_result_t entelea_evt_subscribe(entelea_event_id_t id,
		entelea_event_cb_t fn, void *user)
{
	__ASSERT(!k_is_in_isr(), "evt_subscribe is init/thread only");
	if ((int)id < 0 || id >= ENTELEA_EVT_ID_COUNT || fn == NULL) {
		return ENTELEA_E_PARAM;
	}
	for (int i = 0; i < ENTELEA_EVT_MAX_SUBS; i++) {
		if (subs[id][i].fn == NULL) {
			subs[id][i].fn = fn;
			subs[id][i].user = user;
			return ENTELEA_OK;
		}
	}
	return ENTELEA_E_NOMEM;
}

static void evt_drain_work(struct k_work *work)
{
	ARG_UNUSED(work);
	struct queued_evt q;

	while (k_msgq_get(&evt_q, &q, K_NO_WAIT) == 0) {
		dispatch(&q.evt);
	}
}

static struct k_work drain_work = Z_WORK_INITIALIZER(evt_drain_work);

void entelea_evt_publish(const entelea_event_t *evt)
{
	if (evt == NULL || (int)evt->id < 0 || evt->id >= ENTELEA_EVT_ID_COUNT) {
		return;
	}
	if (k_is_in_isr()) {
		/* data 必须指向静态存储（借用寿命约束，见合同）。 */
		struct queued_evt q = {.evt = *evt};

		q.evt.t_ms = now_ms();
		if (k_msgq_put(&evt_q, &q, K_NO_WAIT) != 0) {
			atomic_inc(&dropped_count);
			return;
		}
		k_work_submit_to_queue(&k_sys_work_q, &drain_work);
		return;
	}
	/* 线程路径同步分发；时间戳与 ISR 路径同由总线加盖（发布瞬间）。 */
	struct queued_evt q = {.evt = *evt};

	q.evt.t_ms = now_ms();
	dispatch(&q.evt);
}

uint32_t entelea_evt_queue_dropped(void)
{
	return (uint32_t)atomic_get(&dropped_count);
}

#ifdef ENTELEA_TEST
void entelea_evt_poll_drain(void)
{
	evt_drain_work(NULL);
}
#endif
