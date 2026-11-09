/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_NETQUEUE_H
#define ENTELEA_NETQUEUE_H

/* R06 avq/pubq 有界传输队列（板级批后 B-1 补交付；合同 framework/
 * services/net/contract.md"传输队列"节）。两条纪律不同的有界队列：
 *  avq（媒体帧外发）= drop-oldest：实时性优先，溢出丢最旧+计数可观；
 *  pubq（发布/命令回执）= 拒新：可靠性优先，满=E_BUSY 显式（不静默丢）。
 * 传输面=可注册 sink（假传输测试闭环；真传输随真网批接入同一形状）。
 * 无定时器：flush 由调用方驱动（确定性——同 R06 合同 9）。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_AVQ_FRAMES 4
#define ENTELEA_AVQ_FRAME_MAX 512 /* 与 R09 媒体帧上限同源（建议值） */
#define ENTELEA_PUBQ_ENTRIES 8
#define ENTELEA_PUBQ_TOPIC_MAX 16
#define ENTELEA_PUBQ_PAYLOAD_MAX 32

/* ---- avq：drop-oldest 媒体帧环 ---- */

/** [thread] 入帧（拷贝入静态池；环满=丢最旧并计 dropped）。帧超限=
 * E_RANGE 整笔拒（不截断——媒体语义整帧交付）。 */
entelea_result_t entelea_avq_push(const uint8_t *frame, uint32_t len);

/** [thread] 出帧到调用方缓冲；空=E_BUSY（非失败——重试面）。 */
entelea_result_t entelea_avq_pop(uint8_t *out, uint32_t max, uint32_t *len);

/** [any] 溢出丢帧计数（观测）。 */
uint32_t entelea_avq_dropped(void);

/* ---- pubq：拒新发布环 ---- */

typedef struct {
	char topic[ENTELEA_PUBQ_TOPIC_MAX];
	uint8_t payload[ENTELEA_PUBQ_PAYLOAD_MAX];
	uint8_t len;
} entelea_pubq_entry_t;

/** [thread] 入发布（拷贝；满=E_BUSY 显式拒；topic/payload 超限=
 * E_PARAM）。 */
entelea_result_t entelea_pubq_post(const char *topic, const uint8_t *payload,
				   uint8_t len);

/** [thread] 取队头（不弹出）；空返回 false。 */
bool entelea_pubq_peek(entelea_pubq_entry_t *out);

/** [thread] 弹出队头（仅经 sink 成功送达后调用——at-least-once 至
 * 传输面；空=E_STATE）。 */
entelea_result_t entelea_pubq_pop(void);

/** [any] 深度（观测）。 */
uint32_t entelea_pubq_depth(void);

/* ---- 传输面（假传输闭环；真传输随真网批） ---- */

typedef void (*entelea_netqueue_sink_t)(void *user, const char *topic,
					const uint8_t *payload, uint8_t len);

/** [boot] 注册传输 sink（NULL=注销——flush 不消费任何侧：avq 停
 *  排、pubq 保持待传输不弹不丢）。 */
void entelea_netqueue_bind_sink(entelea_netqueue_sink_t sink, void *user);

/** [thread] 排空：avq 逐帧经 sink（topic="av"）→pubq 队头经 sink
 * 送达后才弹出。返回处理条数（avq+pubq）。 */
uint32_t entelea_netqueue_flush(void);

#ifdef ENTELEA_TEST
void entelea_netqueue_test_reset(void);
/* avq 帧序观测（顺序断言用：单调递增序号注入测试读取）。 */
uint32_t entelea_netqueue_test_av_delivered(void);
uint32_t entelea_netqueue_test_pub_delivered(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_NETQUEUE_H */
