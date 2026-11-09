/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R06 avq/pubq 有界传输队列实现（B-1；合同 net/contract.md）。
 * avq=drop-oldest 媒体帧环（拷贝入静态池——借用寿命不外泄）；
 * pubq=拒新发布环（满=E_BUSY；弹出仅在 sink 送达后=at-least-once
 * 至传输面）。无定时器——flush 由调用方驱动（确定性）。
 */
#include <string.h>
#include <entelea/netqueue.h>

/* ---- avq ---- */

struct avq_slot {
	uint32_t len;
	uint8_t b[ENTELEA_AVQ_FRAME_MAX];
};

static struct avq_slot avq_ring[ENTELEA_AVQ_FRAMES];
static uint32_t avq_head; /* 弹出侧 */
static uint32_t avq_count;
static uint32_t avq_dropped;

entelea_result_t entelea_avq_push(const uint8_t *frame, uint32_t len)
{
	if (frame == NULL || len == 0) {
		return ENTELEA_E_PARAM;
	}
	if (len > ENTELEA_AVQ_FRAME_MAX) {
		return ENTELEA_E_RANGE; /* 整帧语义：不截断 */
	}
	if (avq_count == ENTELEA_AVQ_FRAMES) {
		avq_head = (avq_head + 1) % ENTELEA_AVQ_FRAMES; /* 丢最旧 */
		avq_count--;
		avq_dropped++;
	}
	uint32_t tail = (avq_head + avq_count) % ENTELEA_AVQ_FRAMES;

	avq_ring[tail].len = len;
	memcpy(avq_ring[tail].b, frame, len);
	avq_count++;
	return ENTELEA_OK;
}

entelea_result_t entelea_avq_pop(uint8_t *out, uint32_t max, uint32_t *len)
{
	if (out == NULL || len == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (avq_count == 0) {
		return ENTELEA_E_BUSY; /* 空=非失败（重试面） */
	}
	if (avq_ring[avq_head].len > max) {
		return ENTELEA_E_RANGE; /* 调用方缓冲不足：整帧语义 */
	}
	*len = avq_ring[avq_head].len;
	memcpy(out, avq_ring[avq_head].b, *len);
	avq_head = (avq_head + 1) % ENTELEA_AVQ_FRAMES;
	avq_count--;
	return ENTELEA_OK;
}

uint32_t entelea_avq_dropped(void)
{
	return avq_dropped;
}

/* ---- pubq ---- */

static entelea_pubq_entry_t pubq_ring[ENTELEA_PUBQ_ENTRIES];
static uint32_t pubq_head;
static uint32_t pubq_count;

entelea_result_t entelea_pubq_post(const char *topic, const uint8_t *payload,
				   uint8_t len)
{
	if (topic == NULL || payload == NULL || len == 0 ||
	    strlen(topic) >= ENTELEA_PUBQ_TOPIC_MAX ||
	    len > ENTELEA_PUBQ_PAYLOAD_MAX) {
		return ENTELEA_E_PARAM;
	}
	if (pubq_count == ENTELEA_PUBQ_ENTRIES) {
		return ENTELEA_E_BUSY; /* 拒新：不静默丢 */
	}
	uint32_t tail = (pubq_head + pubq_count) % ENTELEA_PUBQ_ENTRIES;
	entelea_pubq_entry_t *e = &pubq_ring[tail];

	strcpy(e->topic, topic);
	memcpy(e->payload, payload, len);
	e->len = len;
	pubq_count++;
	return ENTELEA_OK;
}

bool entelea_pubq_peek(entelea_pubq_entry_t *out)
{
	if (pubq_count == 0) {
		return false;
	}
	if (out != NULL) {
		*out = pubq_ring[pubq_head];
	}
	return true;
}

entelea_result_t entelea_pubq_pop(void)
{
	if (pubq_count == 0) {
		return ENTELEA_E_STATE;
	}
	pubq_head = (pubq_head + 1) % ENTELEA_PUBQ_ENTRIES;
	pubq_count--;
	return ENTELEA_OK;
}

uint32_t entelea_pubq_depth(void)
{
	return pubq_count;
}

/* ---- 传输面 ---- */

static entelea_netqueue_sink_t sink;
static void *sink_user;
#ifdef ENTELEA_TEST
static uint32_t av_delivered, pub_delivered;
#endif

void entelea_netqueue_bind_sink(entelea_netqueue_sink_t fn, void *user)
{
	sink = fn;
	sink_user = user;
}

uint32_t entelea_netqueue_flush(void)
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
	/* pubq：sink 送达后才弹（无 sink=不弹不丢——待传输）。 */
	while (sink != NULL && pubq_count > 0) {
		entelea_pubq_entry_t e;

		(void)entelea_pubq_peek(&e);
		sink(sink_user, e.topic, e.payload, e.len);
#ifdef ENTELEA_TEST
		pub_delivered++;
#endif
		if (entelea_pubq_pop() != ENTELEA_OK) {
			break; /* 不可达（单线程域）——防御 */
		}
		n++;
	}
	return n;
}

#ifdef ENTELEA_TEST
void entelea_netqueue_test_reset(void)
{
	memset(avq_ring, 0, sizeof(avq_ring));
	memset(pubq_ring, 0, sizeof(pubq_ring));
	avq_head = avq_count = avq_dropped = 0;
	pubq_head = pubq_count = 0;
	sink = NULL;
	sink_user = NULL;
	av_delivered = pub_delivered = 0;
}

uint32_t entelea_netqueue_test_av_delivered(void)
{
	return av_delivered;
}

uint32_t entelea_netqueue_test_pub_delivered(void)
{
	return pub_delivered;
}
#endif
