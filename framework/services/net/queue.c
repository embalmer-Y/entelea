/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R06 avq/pubq bounded transport queue implementation (B-1; contract
 * net/contract.md).
 * avq = drop-oldest media frame ring (frames are copied into a static pool,
 * so borrowed lifetimes never escape). pubq = reject-new publish ring (full =
 * E_BUSY; an entry is popped after the sink callback, which does not confirm
 * transport results).
 * No timers: flush is driven by the caller (deterministic).
 */
#include <string.h>
#include <entelea/netqueue.h>

/* ---- avq ---- */

struct avq_slot {
	uint32_t len;
	uint8_t b[ENTELEA_AVQ_FRAME_MAX];
};

static struct avq_slot avq_ring[ENTELEA_AVQ_FRAMES];
static uint32_t avq_head; /* Pop side */
static uint32_t avq_count;
static uint32_t avq_dropped;

entelea_result_t
entelea_avq_push(const uint8_t *frame, uint32_t len)
{
	if (frame == NULL || len == 0) {
		return ENTELEA_E_PARAM;
	}
	if (len > ENTELEA_AVQ_FRAME_MAX) {
		return ENTELEA_E_RANGE; /* Whole frame: no truncation */
	}
	if (avq_count == ENTELEA_AVQ_FRAMES) {
		avq_head = (avq_head + 1) % ENTELEA_AVQ_FRAMES; /* Evict */
		avq_count--;
		avq_dropped++;
	}
	uint32_t tail = (avq_head + avq_count) % ENTELEA_AVQ_FRAMES;

	avq_ring[tail].len = len;
	memcpy(avq_ring[tail].b, frame, len);
	avq_count++;
	return ENTELEA_OK;
}

entelea_result_t
entelea_avq_pop(uint8_t *out, uint32_t max, uint32_t *len)
{
	if (out == NULL || len == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (avq_count == 0) {
		return ENTELEA_E_BUSY; /* Empty: not a failure (retry) */
	}
	if (avq_ring[avq_head].len > max) {
		return ENTELEA_E_RANGE; /* Buffer too small: whole frame */
	}
	*len = avq_ring[avq_head].len;
	memcpy(out, avq_ring[avq_head].b, *len);
	avq_head = (avq_head + 1) % ENTELEA_AVQ_FRAMES;
	avq_count--;
	return ENTELEA_OK;
}

uint32_t
entelea_avq_dropped(void)
{
	return avq_dropped;
}

/* ---- pubq ---- */

static entelea_pubq_entry_t pubq_ring[ENTELEA_PUBQ_ENTRIES];
static uint32_t pubq_head;
static uint32_t pubq_count;

entelea_result_t
entelea_pubq_post(const char *topic, const uint8_t *payload, uint8_t len)
{
	if (topic == NULL || payload == NULL || len == 0 ||
	    strlen(topic) >= ENTELEA_PUBQ_TOPIC_MAX ||
	    len > ENTELEA_PUBQ_PAYLOAD_MAX) {
		return ENTELEA_E_PARAM;
	}
	if (pubq_count == ENTELEA_PUBQ_ENTRIES) {
		return ENTELEA_E_BUSY; /* Reject new; no silent drop */
	}
	uint32_t tail = (pubq_head + pubq_count) % ENTELEA_PUBQ_ENTRIES;
	entelea_pubq_entry_t *e = &pubq_ring[tail];

	strcpy(e->topic, topic);
	memcpy(e->payload, payload, len);
	e->len = len;
	pubq_count++;
	return ENTELEA_OK;
}

bool
entelea_pubq_peek(entelea_pubq_entry_t *out)
{
	if (pubq_count == 0) {
		return false;
	}
	if (out != NULL) {
		*out = pubq_ring[pubq_head];
	}
	return true;
}

entelea_result_t
entelea_pubq_pop(void)
{
	if (pubq_count == 0) {
		return ENTELEA_E_STATE;
	}
	pubq_head = (pubq_head + 1) % ENTELEA_PUBQ_ENTRIES;
	pubq_count--;
	return ENTELEA_OK;
}

uint32_t
entelea_pubq_depth(void)
{
	return pubq_count;
}

/* ---- Transport surface ---- */

static entelea_netqueue_sink_t sink;
static void *sink_user;
#ifdef ENTELEA_TEST
static uint32_t av_delivered, pub_delivered;
#endif

void
entelea_netqueue_bind_sink(entelea_netqueue_sink_t fn, void *user)
{
	sink = fn;
	sink_user = user;
}

uint32_t
entelea_netqueue_flush(void)
{
	uint32_t n = 0;
	uint8_t buf[ENTELEA_AVQ_FRAME_MAX];
	uint32_t len = 0;

	if (sink != NULL) {
		while (entelea_avq_pop(buf, sizeof(buf), &len) == ENTELEA_OK) {
			sink(sink_user, "av", buf, len);
#ifdef ENTELEA_TEST
			av_delivered++;
#endif
			n++;
		}
	}
	/* Without a sink, pubq entries stay pending; the callback gives no
	 * delivery acknowledgement. */
	while (sink != NULL && pubq_count > 0) {
		entelea_pubq_entry_t e;

		(void) entelea_pubq_peek(&e);
		sink(sink_user, e.topic, e.payload, e.len);
#ifdef ENTELEA_TEST
		pub_delivered++;
#endif
		if (entelea_pubq_pop() != ENTELEA_OK) {
			break; /* Unreachable (single thread): defensive */
		}
		n++;
	}
	return n;
}

#ifdef ENTELEA_TEST
void
entelea_netqueue_test_reset(void)
{
	memset(avq_ring, 0, sizeof(avq_ring));
	memset(pubq_ring, 0, sizeof(pubq_ring));
	avq_head = avq_count = avq_dropped = 0;
	pubq_head = pubq_count = 0;
	sink = NULL;
	sink_user = NULL;
	av_delivered = pub_delivered = 0;
}

uint32_t
entelea_netqueue_test_av_delivered(void)
{
	return av_delivered;
}

uint32_t
entelea_netqueue_test_pub_delivered(void)
{
	return pub_delivered;
}
#endif
