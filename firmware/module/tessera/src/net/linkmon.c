/* SPDX-License-Identifier: Apache-2.0 */
/* 心跳监视（LLD-ts-net §6）——合同 3 断链判定源 + 合同 8 本地独立判定。
 * 发送：周期〔DEC-22：默认 1000ms〕pub …/sys/hb（payload = 序号 u32 大端）；
 * 监视：host 心跳（…/sys/hb-host）超过〔DEC-22：默认 6〕周期未达 →
 * 模式守卫后 ts_safety_set_link(false)；恢复滞回〔DEC-27：连续 2 次〕才
 * set_link(true)。判定不依赖 router 确认（不依赖外部）；输入/遥测方向不受
 * 影响（合同 3）。迁移经本模块串行上下文（sysworkq tick / 测试直调），无锁。
 *
 * 模式守卫（CG2 · Q-41①，LLD-ts-safety §3a）：仅 orchestrated 部署把链路
 * 判定喂给安全态迁移；standalone 断链/恢复仅留观测事件（本文件事件发布
 * 不变），不调 set_link——本地逻辑照常（standalone 的 boot 链路声明见
 * net/init.c）。 */
#include <string.h>
#include <ts/core.h>
#include <ts/net.h>
#include <ts/safety.h>
#include <ts/store.h>
#include "internal.h"

static bool up;              /* 判定状态（镜像喂给 ts_safety_set_link 的值） */
static bool have_host_hb;
static uint64_t last_host_ms;
static uint32_t streak;      /* 恢复连胜（host 心跳连续到达） */
static uint32_t hb_seq;      /* 已发心跳序号 */
static uint64_t last_send_ms;

/* §3a：orchestrated 才迁移（v1 prov 缺字段 = orchestrated，现行为不变） */
static bool link_migrates_safety(void)
{
	return ts_store_prov()->deploy_mode == TS_DEPLOY_ORCHESTRATED;
}

bool ts_net_link_up(void)
{
	return up;
}

uint32_t ts_net_linkmon_hb_seq(void)
{
	return hb_seq;
}

void ts_net_linkmon_hb_host(uint64_t now_ms)
{
	last_host_ms = now_ms;
	have_host_hb = true;
	streak++;
	if (!up && streak >= (uint32_t)CONFIG_TS_NET_HB_RECOVER) {
		up = true;
		streak = 0;
		if (link_migrates_safety()) {
			ts_safety_set_link(true);
		}
		const ts_evt_t evt = {
			.id = TS_EVT_NET_LINK_UP,
			.t_ms = (uint32_t)now_ms,
		};
		ts_evt_publish(&evt);
	}
}

void ts_net_linkmon_tick(uint64_t now_ms)
{
	const uint32_t period = CONFIG_TS_NET_HB_PERIOD_MS;

	/* 发送侧：CONNECTED 时周期发 hb（host 需要持续观察 cube，与判定状态无关） */
	if (ts_net_transport != NULL && ts_net_state() == TS_NET_CONNECTED &&
	    (now_ms - last_send_ms) >= period) {
		last_send_ms = now_ms;
		hb_seq++;
		char key[64];
		uint8_t p[4] = {
			(uint8_t)(hb_seq >> 24), (uint8_t)(hb_seq >> 16),
			(uint8_t)(hb_seq >> 8), (uint8_t)hb_seq,
		};
		int kw = ts_net_key_hb(key, sizeof(key), false);

		if (kw > 0 && (size_t)kw < sizeof(key)) {
			/* 心跳 = 遥测类尽力而为（DEC-42 QoS 缺省） */
			(void)ts_net_transport->publish(key, p, sizeof(p),
							TS_NET_QOS_BESTEFFORT);
		}
	}
	/* 判定侧：host 心跳超时（elapsed > limit × period）→ 断链 */
	if (have_host_hb && up &&
	    (now_ms - last_host_ms) > (uint64_t)period * CONFIG_TS_NET_HB_MISS_LIMIT) {
		up = false;
		streak = 0;
		if (link_migrates_safety()) {
			ts_safety_set_link(false);
		}
		const ts_evt_t evt = {
			.id = TS_EVT_NET_LINK_DOWN,
			.t_ms = (uint32_t)now_ms,
		};
		ts_evt_publish(&evt);
	}
}

#ifdef CONFIG_TS_TEST
void ts_net_linkmon_test_reset(void)
{
	up = false;
	have_host_hb = false;
	last_host_ms = 0;
	streak = 0;
	hb_seq = 0;
	last_send_ms = 0;
}
#endif
