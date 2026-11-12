/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Event bus (migrated from the old core/events.c; R02 contract:
 * framework/runtime/supervision/contract.md):
 * static registry plus ISR delivery queue (dispatched via sysworkq).
 * Deterministic: dispatch order equals the static registry order (no runtime
 * queue jumping). Changes from the old implementation: the timestamp source
 * moves from the global ts_time_ms to the instance clock binding; the origin of
 * the capacity constants is unchanged (DEC-27).
 */
#include <stdbool.h>
#include <string.h>
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
	uint32_t copied_payload;
	bool copied;
};
K_MSGQ_DEFINE(evt_q, sizeof(struct queued_evt), ENTELEA_EVT_QUEUE_DEPTH, 8);

static atomic_t dropped_count;
static const entelea_clock_t *evt_clock;

entelea_result_t
entelea_evt_bind_clock(const entelea_clock_t *clock)
{
	evt_clock = clock;
	return ENTELEA_OK;
}

/* Publish timestamp: raw read through the bound clock (bypasses the
 * regression bookkeeping of clock_now, which is not atomic; concurrent ISR
 * delivery and thread dispatch on one instance would tear last_ms).
 * Monotonic guard: on read failure or regression, keep the last valid value
 * instead of faking 0. If unbound or never successful -> 0 (observable as
 * unconfigured).
 * Concurrency (second-round review defect 5): ISR publish and thread dispatch
 * access last_good concurrently, and u64 can tear on 32-bit targets. The
 * compare and update go inside an irq_lock micro critical section (ISR may
 * nest). */
static uint64_t
now_ms(void)
{
	static uint64_t last_good;
	uint64_t ms = 0;
	unsigned int key = irq_lock();

	/* Read, compare and update run under the lock (short, non-blocking
	 * read): ISR and thread sampling are serialized, so normal progress is
	 * not misrecorded as a regression; a real regression conservatively
	 * keeps the last valid value. */
	bool ok = evt_clock != NULL && evt_clock->read_ms != NULL &&
		  evt_clock->read_ms(evt_clock->user, &ms) == ENTELEA_OK;

	if (!ok || ms < last_good) {
		ms = last_good;
	} else {
		last_good = ms;
	}
	irq_unlock(key);
	return ms;
}

static void
dispatch(const entelea_event_t *evt)
{
	for (int i = 0; i < ENTELEA_EVT_MAX_SUBS; i++) {
		const struct sub_slot *s = &subs[evt->id][i];

		if (s->fn != NULL) {
			s->fn(evt, s->user);
		}
	}
}

entelea_result_t
entelea_evt_subscribe(entelea_event_id_t id, entelea_event_cb_t fn, void *user)
{
	__ASSERT(!k_is_in_isr(), "evt_subscribe is init/thread only");
	if ((int) id < 0 || id >= ENTELEA_EVT_ID_COUNT || fn == NULL) {
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

static void
evt_drain_work(struct k_work *work)
{
	ARG_UNUSED(work);
	struct queued_evt q;

	while (k_msgq_get(&evt_q, &q, K_NO_WAIT) == 0) {
		if (q.copied) {
			q.evt.data = &q.copied_payload;
		}
		dispatch(&q.evt);
	}
}

static struct k_work drain_work = Z_WORK_INITIALIZER(evt_drain_work);

static entelea_result_t
publish_event(const entelea_event_t *evt, bool copy)
{
	if (evt == NULL || (int) evt->id < 0 ||
	    evt->id >= ENTELEA_EVT_ID_COUNT) {
		return ENTELEA_E_PARAM;
	}
	if (copy && evt->len > sizeof(uint32_t)) {
		return ENTELEA_E_RANGE;
	}
	if (copy && evt->len != 0 && evt->data == NULL) {
		return ENTELEA_E_PARAM;
	}
	struct queued_evt q = { .evt = *evt };

	if (copy && evt->len != 0) {
		memcpy(&q.copied_payload, evt->data, evt->len);
		q.copied = true;
	}
	if (k_is_in_isr()) {
		q.evt.t_ms = now_ms();
		if (k_msgq_put(&evt_q, &q, K_NO_WAIT) != 0) {
			atomic_inc(&dropped_count);
			return ENTELEA_E_BUSY;
		}
		k_work_submit_to_queue(&k_sys_work_q, &drain_work);
		return ENTELEA_OK;
	}
	/* Thread path dispatches synchronously; like the ISR path, the bus
	 * stamps the time at publish. */
	if (q.copied) {
		q.evt.data = &q.copied_payload;
	}
	q.evt.t_ms = now_ms();
	dispatch(&q.evt);
	return ENTELEA_OK;
}

void
entelea_evt_publish(const entelea_event_t *evt)
{
	(void) publish_event(evt, false);
}

entelea_result_t
entelea_evt_publish_copy(const entelea_event_t *evt)
{
	return publish_event(evt, true);
}

uint32_t
entelea_evt_queue_dropped(void)
{
	return (uint32_t) atomic_get(&dropped_count);
}

#ifdef ENTELEA_TEST
void
entelea_evt_poll_drain(void)
{
	evt_drain_work(NULL);
}
#endif
