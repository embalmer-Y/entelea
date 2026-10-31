/* SPDX-License-Identifier: Apache-2.0 */
/* av 分片通道队列（DEC-47①②，MD1.2g）：APP 线程入队（信封封装 CBOR），
 * ts-net 周期体单线程冲刷（打拍 ≥MIN_GAP + 分片重试 ≤RETRY_MAX×DELAY——
 * 常量全部 DEC-47② 实测值，经 Kconfig 出处可查）。单发送线程 = zenoh-pico
 * 非线程安全面的并发规避（与 pubq 同语义；锁内出入队、锁外发送/睡眠）。
 * DOWN 期丢弃 + 计数（帧分片尽力而为；消费端按 crc/帧序丢弃残帧）。
 * 信封 = canonical 5 对 map：fid/cid/n/crc(IEEE crc32)/d(bstr)。 */
#include <string.h>
#include <ts/core.h>
#include <ts/net.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include "internal.h"

#define AVQ_DEPTH CONFIG_TS_NET_AVQ_DEPTH
#define AVQ_PAYLOAD_MAX (CONFIG_TS_NET_PUBLISH_MAX_BYTES + 48) /* 信封开销上界 */

struct avq_ent {
	uint8_t payload[AVQ_PAYLOAD_MAX];
	uint16_t len;
	uint16_t app_id;
};

static struct avq_ent ents[AVQ_DEPTH];
static uint8_t head, count;
static uint32_t dropped;
static K_MUTEX_DEFINE(avq_lock);

ts_res_t ts_net_avq_push(uint16_t app_id, uint32_t fid, uint32_t cid,
			 uint32_t n_chunks, const uint8_t *data, uint32_t len)
{
	if (data == NULL || len == 0 || len > CONFIG_TS_NET_PUBLISH_MAX_BYTES) {
		return TS_E_PARAM;
	}
	if (n_chunks == 0 || n_chunks > 4096 || cid >= n_chunks) {
		return TS_E_PARAM; /* 分片序号域（信封完整性；hal 层同校验） */
	}
	k_mutex_lock(&avq_lock, K_FOREVER);
	if (count >= AVQ_DEPTH) {
		dropped++; /* 队满 = 背压（调用方稍后重发该分片） */
		k_mutex_unlock(&avq_lock);
		return TS_E_BUSY;
	}
	struct avq_ent *e = &ents[(head + count) % AVQ_DEPTH];
	size_t pos = 0;
	const uint32_t crc = crc32_ieee(data, len);

	bool ok = ts_cbor_put_map(e->payload, sizeof(e->payload), &pos, 5) &&
		  ts_cbor_put_tstr(e->payload, sizeof(e->payload), &pos, "fid") &&
		  ts_cbor_put_uint(e->payload, sizeof(e->payload), &pos, fid) &&
		  ts_cbor_put_tstr(e->payload, sizeof(e->payload), &pos, "cid") &&
		  ts_cbor_put_uint(e->payload, sizeof(e->payload), &pos, cid) &&
		  ts_cbor_put_tstr(e->payload, sizeof(e->payload), &pos, "n") &&
		  ts_cbor_put_uint(e->payload, sizeof(e->payload), &pos, n_chunks) &&
		  ts_cbor_put_tstr(e->payload, sizeof(e->payload), &pos, "crc") &&
		  ts_cbor_put_uint(e->payload, sizeof(e->payload), &pos, crc) &&
		  ts_cbor_put_tstr(e->payload, sizeof(e->payload), &pos, "d") &&
		  ts_cbor_put_bstr(e->payload, sizeof(e->payload), &pos, data, len);

	if (!ok) {
		k_mutex_unlock(&avq_lock);
		return TS_E_PARAM; /* 信封超容量（len 已限 ≤MAX——防御性兜底） */
	}
	e->len = (uint16_t)pos;
	e->app_id = app_id;
	count++;
	k_mutex_unlock(&avq_lock);
	return TS_OK;
}

void ts_net_avq_flush(void)
{
	if (ts_net_state() != TS_NET_CONNECTED || ts_net_transport == NULL) {
		/* DOWN 期自弃（pubq 同语义）+ 计数 */
		k_mutex_lock(&avq_lock, K_FOREVER);
		dropped += count;
		head = 0;
		count = 0;
		k_mutex_unlock(&avq_lock);
		return;
	}
	bool first = true;

	for (;;) {
		struct avq_ent e;
		char key[64];

		k_mutex_lock(&avq_lock, K_FOREVER);
		if (count == 0) {
			k_mutex_unlock(&avq_lock);
			return;
		}
		e = ents[head]; /* 锁内出队；发送在锁外（传输阻塞不反压入队方） */
		head = (head + 1) % AVQ_DEPTH;
		count--;
		k_mutex_unlock(&avq_lock);

		if (ts_net_key_av(key, sizeof(key), e.app_id) <= 0) {
			k_mutex_lock(&avq_lock, K_FOREVER);
			dropped++;
			k_mutex_unlock(&avq_lock);
			continue;
		}
		if (!first) {
			k_sleep(K_MSEC(CONFIG_TS_NET_PUBLISH_MIN_GAP_MS)); /* 打拍 */
		}
		bool sent = false;

		for (int r = 0; r < CONFIG_TS_NET_PUBLISH_RETRY_MAX; r++) {
			/* QoS = BESTEFFORT（DEC-42 语义）。注：V2-1 实验期曾疑
			 * DROP 拥塞全丢——后定位根因为 zenoh glue 的
			 * use-after-overwrite（static_buf 别名 + 环形槽覆盖，
			 * zenoh.c 已改拷贝语义根治；64B 档 1842 chunks 零损坏
			 * 实证），BESTEFFORT 恢复。 */
			if (ts_net_transport->publish(key, e.payload, e.len,
						      TS_NET_QOS_BESTEFFORT) == TS_OK) {
				sent = true;
				break;
			}
			if (r < CONFIG_TS_NET_PUBLISH_RETRY_MAX - 1) {
				k_sleep(K_MSEC(CONFIG_TS_NET_PUBLISH_RETRY_DELAY_MS));
			}
		}
		if (!sent) {
			/* 重试耗尽：丢弃并止冲刷（限本 tick 时长——net_wq 还有
			 * session/linkmon 职责，不为一条坏分片整拍停摆） */
			k_mutex_lock(&avq_lock, K_FOREVER);
			dropped++;
			k_mutex_unlock(&avq_lock);
			return;
		}
		first = false;
	}
}

uint32_t ts_net_avq_dropped(void)
{
	return dropped;
}

void ts_net_avq_reset(void)
{
	k_mutex_lock(&avq_lock, K_FOREVER);
	head = 0;
	count = 0;
	dropped = 0;
	k_mutex_unlock(&avq_lock);
}
