/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 事件总线（LLD-ts-core §4）：静态注册表 + ISR 投递队列（sysworkq 分发）。
 * 确定性：分发顺序 = 注册表静态顺序（禁运行期插队）。
 */
#include <ts/core.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct sub_slot {
	ts_evt_cb_t fn;
	void *user;
};

/* 每事件类型订阅上限 CONFIG_TS_CORE_MAX_SUBS（来源: DEC-27）；满 = TS_E_NOMEM（启动期暴露） */
static struct sub_slot subs[TS_EVT_ID_COUNT][CONFIG_TS_CORE_MAX_SUBS];

/* ISR 投递队列：深度 CONFIG_TS_CORE_EVT_QUEUE_DEPTH（来源: DEC-27）；
 * 满 = 丢弃并计数留痕（观测损失，非安全损失）。 */
struct queued_evt {
	ts_evt_t evt;
};
K_MSGQ_DEFINE(evt_q, sizeof(struct queued_evt), CONFIG_TS_CORE_EVT_QUEUE_DEPTH, 8);

static atomic_t dropped_count;

static void dispatch(const ts_evt_t *evt)
{
	for (int i = 0; i < CONFIG_TS_CORE_MAX_SUBS; i++) {
		const struct sub_slot *s = &subs[evt->id][i];
		if (s->fn != NULL) {
			s->fn(evt, s->user);
		}
	}
}

ts_res_t ts_evt_subscribe(ts_evt_id_t id, void (*fn)(const ts_evt_t *, void *), void *user)
{
	__ASSERT(!k_is_in_isr(), "ts_evt_subscribe is init/thread only");
	if (id >= TS_EVT_ID_COUNT || fn == NULL) {
		return TS_E_PARAM;
	}
	for (int i = 0; i < CONFIG_TS_CORE_MAX_SUBS; i++) {
		if (subs[id][i].fn == NULL) {
			subs[id][i].fn = fn;
			subs[id][i].user = user;
			return TS_OK;
		}
	}
	return TS_E_NOMEM;
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

void ts_evt_publish(const ts_evt_t *evt)
{
	if (evt == NULL || evt->id >= TS_EVT_ID_COUNT) {
		return;
	}
	if (k_is_in_isr()) {
		/* data 必须指向静态存储（约束见 core.h） */
		struct queued_evt q = {.evt = *evt};

		if (k_msgq_put(&evt_q, &q, K_NO_WAIT) != 0) {
			atomic_inc(&dropped_count);
			return;
		}
		k_work_submit_to_queue(&k_sys_work_q, &drain_work);
		return;
	}
	dispatch(evt);
}

uint32_t ts_evt_queue_dropped(void)
{
	return (uint32_t)atomic_get(&dropped_count);
}

#ifdef CONFIG_TS_TEST
void ts_evt_poll_drain(void)
{
	evt_drain_work(NULL);
}
#endif
