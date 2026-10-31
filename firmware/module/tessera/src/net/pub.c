/* SPDX-License-Identifier: Apache-2.0 */
/* 发布面（LLD-ts-net §5）：遥测周期快照 + 事件选择性外发 → pubq（尽力而为，
 * 不阻塞控制路径）。DOWN 期丢弃/满丢最旧由 pubq 统一语义处理。
 * 事件外发 = 合同 5（estop 事后补发可见）/合同 10（越权留痕对外可见）的
 * 观测面；只观测不改值（不影响任何控制路径——合同 3）。
 * 信封 v1（DEC-42）：事件 = map{ver:1, kind:32+evt_id, t_ms, wall_ms, …}；
 * 遥测 = map{ver:1, kind:96, dev, value_u, wall_ms}；kind 注册表唯一权威 =
 * LLD-ts-net §4.4（Agent keys.py 镜像）。QoS 映射（DEC-42）：安全事件
 * （estop/安全态迁移/越权）= 阻塞式高优先级；其余与遥测 = 丢弃式尽力而为。 */
#include <string.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/net.h>
#include <ts/power.h>
#include <ts/safety.h>
#include <zephyr/kernel.h>
#include "internal.h"

/* kind 注册表区间断言（LLD §4.4：事件 32-95，32+TS_EVT_ID） */
BUILD_ASSERT(32 + TS_EVT_ID_COUNT - 1 <= 95, "evt kind overflow: 32+id must stay <=95");

#define KIND_EVT_BASE 32
#define KIND_TELEMETRY 96

/* ---- 事件订阅 → pubq ------------------------------------------------------ */

static ts_net_qos_t evt_qos(ts_evt_id_t id)
{
	switch (id) {
	case TS_EVT_ESTOP:
	case TS_EVT_SAFE_STATE_CHANGED:
	case TS_EVT_PERM_DENIED:
	case TS_EVT_ARB_CHANGED: /* CG3 · 合同 11：控制权切换 = 安全类（留痕必达优先） */
	case TS_EVT_TRUST_UPDATED: /* EC2 · Q-43④：信任链变更 = 安全类（同上） */
	case TS_EVT_PENDING_INSTALL: /* F3 · DEC-62①：安装闸挂起 = 留痕必达（确认窗起点） */
		return TS_NET_QOS_SAFETY; /* DEC-42 QoS 映射 */
	default:
		return TS_NET_QOS_BESTEFFORT;
	}
}

static void push_evt(ts_evt_id_t id, uint64_t t_ms, const char *uid_key_suffix,
		     const uint8_t *extra, size_t extra_len, uint32_t extra_pairs)
{
	char key[64];
	int kw = ts_net_key_evt(key, sizeof(key), uid_key_suffix);

	if (kw <= 0 || (size_t)kw >= sizeof(key)) {
		kw = ts_net_key_sys(key, sizeof(key), "event");
		if (kw <= 0 || (size_t)kw >= sizeof(key)) {
			return;
		}
	}
	/* payload = map{ver, kind=32+id, t_ms, wall_ms, extra?}（DEC-42）。
	 * extra = extra_pairs 个**扁平 key:value 对**的原始 CBOR 字节
	 *（append 进外层 map——SAFE_STATE_CHANGED 一对 / periph 两对 /
	 * 预算三对）。128B = 信封头（~45B）+ 最宽 extra（预算 ~51B）+ 余量。 */
	uint8_t buf[128];
	size_t p = 0;
	bool ok = ts_cbor_put_map(buf, sizeof(buf), &p, 4 + extra_pairs) &&
		  ts_cbor_put_tstr(buf, sizeof(buf), &p, "ver") &&
		  ts_cbor_put_uint(buf, sizeof(buf), &p, 1) &&
		  ts_cbor_put_tstr(buf, sizeof(buf), &p, "kind") &&
		  ts_cbor_put_uint(buf, sizeof(buf), &p, KIND_EVT_BASE + (uint32_t)id) &&
		  ts_cbor_put_tstr(buf, sizeof(buf), &p, "t_ms") &&
		  ts_cbor_put_uint(buf, sizeof(buf), &p, t_ms) &&
		  ts_cbor_put_tstr(buf, sizeof(buf), &p, "wall_ms") &&
		  ts_cbor_put_uint(buf, sizeof(buf), &p, ts_time_wall_ms());

	if (ok && extra != NULL && extra_len > 0 && extra_len <= sizeof(buf) - p &&
	    p + extra_len <= sizeof(buf)) {
		memcpy(buf + p, extra, extra_len);
		p += extra_len;
	}
	if (ok) {
		(void)ts_net_pubq_push_qos(key, buf, (uint32_t)p, evt_qos(id));
	}
}

static void on_evt(const ts_evt_t *evt, void *user)
{
	ARG_UNUSED(user);
	switch (evt->id) {
	case TS_EVT_ESTOP: /* 事后补发观测（合同 5；estop 动作本身不经总线） */
		push_evt(evt->id, evt->t_ms, "sys", NULL, 0, 0);
		break;
	case TS_EVT_SAFE_STATE_CHANGED: {
		/* payload: ts_safe_state_evt_t（safety.h——uid + 新态） */
		const ts_safe_state_evt_t *pl = evt->data;
		uint8_t extra[24];
		size_t p = 0;

		if (ts_cbor_put_tstr(extra, sizeof(extra), &p, "state") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, (uint64_t)pl->new_state)) {
			push_evt(evt->id, evt->t_ms, pl->uid, extra, p, 1);
		}
		break;
	}
	case TS_EVT_PERIPH_ATTACH:
	case TS_EVT_PERIPH_DETACH: {
		/* payload: ts_periph_evt_t（core.h——uid + periph 类别）。
		 * F-1（docs/history/review-archive）：插拔归因外发（LLD-ts-periph §3"key 下线
		 * 通告"落点）；QoS = 尽力而为（DEC-42 安全类集合不含插拔）。 */
		const ts_periph_evt_t *pl = evt->data;
		uint8_t extra[48];
		size_t p = 0;

		if (ts_cbor_put_tstr(extra, sizeof(extra), &p, "uid") &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p, pl->uid) &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p, "pkind") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, pl->kind)) {
			push_evt(evt->id, evt->t_ms, pl->uid, extra, p, 2);
		}
		break;
	}
	case TS_EVT_POWER_BUDGET: {
		/* payload: ts_pwr_budget_evt_t（core.h——超预算拒绝归因，
		 * LLD-ts-power §3）；QoS = 尽力而为（同上，DEC-42 类属）。 */
		const ts_pwr_budget_evt_t *pl = evt->data;
		uint8_t extra[64];
		size_t p = 0;

		if (ts_cbor_put_tstr(extra, sizeof(extra), &p, "requested_ma") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, pl->requested_ma) &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p, "used_ma") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, pl->used_ma) &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p, "budget_ma") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, pl->budget_ma)) {
			push_evt(evt->id, evt->t_ms, "sys", extra, p, 3);
		}
		break;
	}
	case TS_EVT_ARB_CHANGED: {
		/* payload: ts_arb_evt_t（safety.h——新/旧席主，CG3 · 合同 11）。
		 * 路由 …/sys/event，extra = {"arb", "old"}（owner 字符串化——
		 * 消费端零枚举耦合）。 */
		const ts_arb_evt_t *pl = evt->data;
		uint8_t extra[32];
		size_t p = 0;

		if (ts_cbor_put_tstr(extra, sizeof(extra), &p, "arb") &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p,
				     pl->owner == TS_ARB_LOCAL ? "local" : "remote") &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p, "old") &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p,
				     pl->old == TS_ARB_LOCAL ? "local" : "remote")) {
			push_evt(evt->id, evt->t_ms, "sys", extra, p, 2);
		}
		break;
	}
	case TS_EVT_TRUST_UPDATED: {
		/* payload: ts_trust_evt_t（core.h——新 ver + 键数，EC2 · Q-43④）。
		 * 路由 …/sys/event，extra = {"ver"}——消费端零结构耦合。 */
		const ts_trust_evt_t *pl = evt->data;
		uint8_t extra[16];
		size_t p = 0;

		if (ts_cbor_put_tstr(extra, sizeof(extra), &p, "ver") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, pl->ver)) {
			push_evt(evt->id, evt->t_ms, "sys", extra, p, 1);
		}
		break;
	}
	case TS_EVT_PENDING_INSTALL: /* F3 · DEC-62① 四态同 payload {slot,entry} */
	case TS_EVT_INSTALL_CONFIRMED:
	case TS_EVT_INSTALL_REJECTED:
	case TS_EVT_INSTALL_TIMEOUT: {
		const ts_pending_evt_t *pl = evt->data;
		uint8_t extra[24];
		size_t p = 0;

		if (ts_cbor_put_tstr(extra, sizeof(extra), &p, "slot") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, pl->slot) &&
		    ts_cbor_put_tstr(extra, sizeof(extra), &p, "entry") &&
		    ts_cbor_put_uint(extra, sizeof(extra), &p, pl->entry)) {
			push_evt(evt->id, evt->t_ms, "sys", extra, p, 2);
		}
		break;
	}
	case TS_EVT_PERM_DENIED: /* 合同 10 留痕外发 */
	case TS_EVT_INPUT_CHANGED: /* DR-02 输入变化 */
	default:
		/* 通用：ver/kind/t_ms（detail 原始结构非稳定编码，不外发——防架构相关字节） */
		push_evt(evt->id, evt->t_ms, "sys", NULL, 0, 0);
		break;
	}
}

ts_res_t ts_net_pub_init(void)
{
	static bool subscribed;
	ts_res_t r = TS_OK;

	if (subscribed) {
		return TS_OK;
	}
	/* 订阅容量 = CONFIG_TS_CORE_MAX_SUBS **按事件类型分桶**（events.c 的
	 * subs[id][N]，各类型独立 4 位——F-1 修复时更正早前"总量 4"误读）；
	 * 本处九类订阅各占一桶一位，单桶满员属装配错误（TS_E_NOMEM 如实上报） */
	const ts_evt_id_t ids[] = {
		TS_EVT_ESTOP, TS_EVT_SAFE_STATE_CHANGED,
		TS_EVT_PERM_DENIED, TS_EVT_INPUT_CHANGED,
		TS_EVT_PERIPH_ATTACH, TS_EVT_PERIPH_DETACH,
		TS_EVT_POWER_BUDGET,
		TS_EVT_ARB_CHANGED, /* CG3 · 合同 11：控制权切换留痕外发 */
		TS_EVT_RESOURCE_WRITTEN, /* CG5-B：资源推送完成留痕（观测类） */
		TS_EVT_TRUST_UPDATED, /* EC2 · Q-43④：信任根表更新留痕（安全类） */
		TS_EVT_PENDING_INSTALL, /* F3 · DEC-62①：安装闸四态外发（挂起 = 安全类） */
		TS_EVT_INSTALL_CONFIRMED,
		TS_EVT_INSTALL_REJECTED,
		TS_EVT_INSTALL_TIMEOUT,
	};

	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		r = ts_evt_subscribe(ids[i], on_evt, NULL);
		if (r != TS_OK) {
			return r;
		}
	}
	subscribed = true;
	return r;
}

/* ---- 遥测周期快照（§5：实例值 + 周期合流）-------------------------------- */

static ts_ch_kind_t ch_kind_of(ts_dev_kind_t k)
{
	switch (k) {
	case TS_DEV_GPIO_OUT:
		return TS_CH_GPIO;
	case TS_DEV_PWM:
		return TS_CH_PWM;
	case TS_DEV_POWER:
		return TS_CH_POWER;
	default:
		return TS_CH_KIND_COUNT;
	}
}

void ts_net_pub_telem(uint64_t now_ms)
{
	ARG_UNUSED(now_ms);
	for (size_t i = 0; i < ts_hal_dev_count(); i++) {
		const ts_hal_dev_desc_t *d = ts_hal_dev_get(i);
		ts_ch_kind_t ck = (d != NULL) ? ch_kind_of(d->kind) : TS_CH_KIND_COUNT;

		if (d == NULL || ck == TS_CH_KIND_COUNT) {
			continue; /* 输入实例走事件路径（DR-02），不进快照 */
		}
		ts_out_value_t v;

		if (ts_safety_readback(d->uid, &v) != TS_OK) {
			continue; /* hal 实例未必都有 safety 通道（如电源桩） */
		}
		/* payload = map{ver, kind:96, dev, value_u, wall_ms}（DEC-42） */
		uint8_t buf[64];
		size_t p = 0;

		if (!ts_cbor_put_map(buf, sizeof(buf), &p, 5) ||
		    !ts_cbor_put_tstr(buf, sizeof(buf), &p, "ver") ||
		    !ts_cbor_put_uint(buf, sizeof(buf), &p, 1) ||
		    !ts_cbor_put_tstr(buf, sizeof(buf), &p, "kind") ||
		    !ts_cbor_put_uint(buf, sizeof(buf), &p, KIND_TELEMETRY) ||
		    !ts_cbor_put_tstr(buf, sizeof(buf), &p, "dev") ||
		    !ts_cbor_put_uint(buf, sizeof(buf), &p, (uint64_t)d->kind) ||
		    !ts_cbor_put_tstr(buf, sizeof(buf), &p, "value_u") ||
		    !ts_cbor_put_uint(buf, sizeof(buf), &p, ts_value_encode(ck, v)) ||
		    !ts_cbor_put_tstr(buf, sizeof(buf), &p, "wall_ms") ||
		    !ts_cbor_put_uint(buf, sizeof(buf), &p, ts_time_wall_ms())) {
			continue;
		}
		char key[64];
		int kw = ts_net_key_tel(key, sizeof(key), d->uid);

		if (kw > 0 && (size_t)kw < sizeof(key)) {
			(void)ts_net_pubq_push(key, buf, (uint32_t)p); /* 遥测 = 尽力而为缺省 */
		}
	}
#ifdef CONFIG_TS_POWER
	/* 功率预算快照（kind 97——遥测区间分配，LLD-ts-net §4.4；M3b） */
	{
		ts_power_budget_t b;
		uint8_t buf[72];
		size_t p = 0;

		ts_power_budget_snapshot(&b);
		if (ts_cbor_put_map(buf, sizeof(buf), &p, 6) &&
		    ts_cbor_put_tstr(buf, sizeof(buf), &p, "ver") &&
		    ts_cbor_put_uint(buf, sizeof(buf), &p, 1) &&
		    ts_cbor_put_tstr(buf, sizeof(buf), &p, "kind") &&
		    ts_cbor_put_uint(buf, sizeof(buf), &p, 97) &&
		    ts_cbor_put_tstr(buf, sizeof(buf), &p, "budget_ma") &&
		    ts_cbor_put_uint(buf, sizeof(buf), &p, b.budget_ma) &&
		    ts_cbor_put_tstr(buf, sizeof(buf), &p, "used_ma") &&
		    ts_cbor_put_uint(buf, sizeof(buf), &p, b.used_ma) &&
		    ts_cbor_put_tstr(buf, sizeof(buf), &p, "peak_ma") &&
		    ts_cbor_put_uint(buf, sizeof(buf), &p, b.peak_ma) &&
		    ts_cbor_put_tstr(buf, sizeof(buf), &p, "wall_ms") &&
		    ts_cbor_put_uint(buf, sizeof(buf), &p, ts_time_wall_ms())) {
			char key[64];
			int kw = ts_net_key_sys(key, sizeof(key), "power");

			if (kw > 0 && (size_t)kw < sizeof(key)) {
				(void)ts_net_pubq_push(key, buf, (uint32_t)p);
			}
		}
	}
#endif
}
