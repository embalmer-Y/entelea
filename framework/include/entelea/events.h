/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_EVENTS_H
#define ENTELEA_EVENTS_H

/* R02 synchronous event bus contract: see
 * framework/runtime/supervision/contract.md.
 * Dispatch order is the static registry order (no runtime queue-jumping). ISR
 * posts go through the queue and are dispatched by sysworkq. The data of a
 * posted event must point to static storage (a borrowed-lifetime constraint,
 * with no mechanical check). */

#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>
#include <entelea/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R02 event subset. Later batches append as needed and do not change existing
 * numbers. */
typedef enum {
	ENTELEA_EVT_BOOT_STEP,
	ENTELEA_EVT_BOOT_DONE,
	ENTELEA_EVT_ESTOP, /* backfilled; the estop action is not queued */
	ENTELEA_EVT_SAFE_STATE_CHANGED,
	ENTELEA_EVT_WDT_WARN,
	ENTELEA_EVT_PERM_DENIED, /* R04: denial record (app_id+detail) */
	ENTELEA_EVT_ARB_CHANGED, /* R04: arbiter seat change (old/owner/t) */
	ENTELEA_EVT_ID_COUNT,
} entelea_event_id_t;

typedef struct {
	entelea_event_id_t id;
	uint64_t t_ms;
	const void *data;
	size_t len;
} entelea_event_t;

typedef void (*entelea_event_cb_t)(const entelea_event_t *evt, void *user);

/* ENTELEA_EVT_BOOT_STEP payload */
typedef struct {
	uint8_t idx;
	int32_t result;
	const char *name;
} entelea_boot_step_evt_t;

/* ENTELEA_EVT_ESTOP payload (a backfilled event carries the trigger time
 * recorded by the ISR) */
typedef struct {
	uint32_t t_ms;
} entelea_estop_evt_t;

/* ENTELEA_EVT_WDT_WARN payload */
typedef struct {
	int32_t src; /* entelea_wdt_src_t value */
	uint32_t last_feed;
} entelea_wdt_warn_evt_t;

/* Binds the instance clock (the source of publish timestamps). While unbound,
 * events are published with t_ms = 0; no default time base is fabricated. Other
 * subscribe/publish semantics are unchanged. Binding NULL restores the unbound
 * state. */
entelea_result_t
entelea_evt_bind_clock(const entelea_clock_t *clock);

/** [thread] Registers only during init. Each event has a capacity of
 * ENTELEA_EVT_MAX_SUBS subscribers; when full, returns ENTELEA_E_NOMEM. */
entelea_result_t
entelea_evt_subscribe(entelea_event_id_t id, entelea_event_cb_t fn, void *user);

/** [thread] Dispatches synchronously (registry static order). [ISR] Posts to a
 * queue of depth ENTELEA_EVT_QUEUE_DEPTH, dispatched by sysworkq; when full,
 * the event is dropped and counted as an observation loss. The data of an ISR
 * post must point to static storage. */
void
entelea_evt_publish(const entelea_event_t *evt);

/** [any] Copies a payload of up to 4 bytes at publish time; valid during the
 * callback. A full queue returns E_BUSY and is counted as a drop; an oversized
 * payload returns E_RANGE; invalid arguments return E_PARAM. */
entelea_result_t
entelea_evt_publish_copy(const entelea_event_t *evt);

/** [any] Cumulative count of queue-overflow drops. Observation only; this
 * count is not cleared. */
uint32_t
entelea_evt_queue_dropped(void);

/* ENTELEA_TEST builds only: synchronously drains and dispatches the ISR post
 * queue (for test observation). */
#ifdef ENTELEA_TEST
void
entelea_evt_poll_drain(void);
#endif

/* Capacity source: DEC-27 (legacy LLD-ts-core sections 4 and 6). R02 uses
 * compile-time constants. Kconfig is not provided before the board-level
 * freeze (D12). */
#define ENTELEA_EVT_MAX_SUBS 4
#define ENTELEA_EVT_QUEUE_DEPTH 16

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_EVENTS_H */
