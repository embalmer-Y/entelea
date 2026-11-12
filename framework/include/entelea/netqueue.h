/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_NETQUEUE_H
#define ENTELEA_NETQUEUE_H

/* R06 avq/pubq bounded transport queues (B-1 delivery, completed after the
 * board-level batch; contract framework/services/net/contract.md, section
 * "transport queues"). The two bounded queues follow different disciplines:
 *  avq (media frame egress) = drop-oldest: real-time takes priority; on
 *     overflow the oldest frame is dropped and the count is observable;
 *  pubq (publish/command receipts) = reject-new: reliability takes priority;
 *     when full, it returns E_BUSY explicitly (never silently dropped).
 * The transport side is a registrable sink (closed loop with a fake transport
 * in tests; the real transport later joins the same shape in the real-network
 * batch). There are no timers: flush is driven by the caller (determinism, per
 * R06 contract 9). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_AVQ_FRAMES 4
#define ENTELEA_AVQ_FRAME_MAX 512 /* same as R09 frame limit (suggested) */
#define ENTELEA_PUBQ_ENTRIES 8
#define ENTELEA_PUBQ_TOPIC_MAX 16
#define ENTELEA_PUBQ_PAYLOAD_MAX 32

/* ---- avq: drop-oldest media frame ring ---- */

/** [thread] Pushes a frame (copied). When the ring is full, the oldest
 * frame is dropped and counted in dropped. A frame over the limit returns
 * E_RANGE and is rejected whole (never truncated: media delivers whole
 * frames). */
entelea_result_t
entelea_avq_push(const uint8_t *frame, uint32_t len);

/** [thread] Pops a frame into the caller's buffer. When empty, returns E_BUSY
 * (not a failure; retry later). */
entelea_result_t
entelea_avq_pop(uint8_t *out, uint32_t max, uint32_t *len);

/** [any] Overflow drop count (for observation). */
uint32_t
entelea_avq_dropped(void);

/* ---- pubq: reject-new publish ring ---- */

typedef struct {
	char topic[ENTELEA_PUBQ_TOPIC_MAX];
	uint8_t payload[ENTELEA_PUBQ_PAYLOAD_MAX];
	uint8_t len;
} entelea_pubq_entry_t;

/** [thread] Posts a publish entry (copied). When full, returns E_BUSY
 * explicitly. A topic or payload over the limit returns E_PARAM. */
entelea_result_t
entelea_pubq_post(const char *topic, const uint8_t *payload, uint8_t len);

/** [thread] Peeks at the head entry without removing it. Returns false if
 * empty. */
bool
entelea_pubq_peek(entelea_pubq_entry_t *out);

/** [thread] Pops the head entry (empty returns E_STATE). The sink returns no
 * acknowledgment, so network delivery cannot be determined from it. */
entelea_result_t
entelea_pubq_pop(void);

/** [any] Depth (for observation). */
uint32_t
entelea_pubq_depth(void);

/* ---- Transport surface (fake transport loop; real transport later) ---- */

typedef void (*entelea_netqueue_sink_t)(void *user, const char *topic,
					const uint8_t *payload, uint32_t len);

/** [boot] Registers the transport sink. NULL unregisters it; flush then
 *  consumes nothing: avq stops draining, and pubq keeps its entries pending
 *  (not popped, not dropped). */
void
entelea_netqueue_bind_sink(entelea_netqueue_sink_t sink, void *user);

/** [thread] Drains: avq frames go through the sink one at a time (topic
 * "av"), then the pubq head goes through the sink and is popped after its
 * callback. Returns the number of callbacks, which does not mean that
 * transmission or remote delivery succeeded. */
uint32_t
entelea_netqueue_flush(void);

#ifdef ENTELEA_TEST
void
entelea_netqueue_test_reset(void);
/* The legacy "delivered" names count callbacks only and do not mean actual
 * delivery. */
uint32_t
entelea_netqueue_test_av_delivered(void);
uint32_t
entelea_netqueue_test_pub_delivered(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_NETQUEUE_H */
