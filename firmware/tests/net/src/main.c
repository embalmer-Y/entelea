/* SPDX-License-Identifier: Apache-2.0 */
/* ts-net L1/L2 测试（LLD-ts-net §8）：key 构造 / 退避表 / pubq 语义 /
 * 会话状态机 / linkmon→safety 断链-恢复集成（虚拟时钟确定性）。 */
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/net.h>
#include <ts/periph.h>
#include <ts/safety.h>
#include <ts/store.h>
/* 最小 CBOR 编解码（模块内部 API——测试构造请求/解回执复用同一实现） */
#include "../../../module/tessera/src/net/internal.h"

/* ---- 注入桩传输 ---------------------------------------------------------- */

static int fake_open_fail;
static bool open_grace_seen; /* fake_open 调用体内的 wdt 豁免窗观测（test_17 消费） */
static int fake_close_calls;
static int fake_pub_calls;
static bool fake_up_v;
static int fake_pub_fail; /* >0 = 接下来 N 次 publish 失败（重试路径注入） */
static char last_key[64];
static uint8_t last_payload[128]; /* = pubq PAYLOAD_MAX（F-1 预算事件 ~85B） */
static uint32_t last_len;
static ts_net_qos_t last_qos;
static char prev_key[64];
static uint8_t prev_payload[128];
static uint32_t prev_len;

static ts_res_t fake_open(void)
{
	open_grace_seen = ts_wdt_grace_active(TS_WDT_NET);
	return fake_open_fail ? TS_E_IO : TS_OK;
}
static void fake_close(void)
{
	fake_close_calls++;
}
static ts_res_t fake_publish(const char *key, const uint8_t *p, uint32_t l,
			     ts_net_qos_t qos)
{
	if (fake_pub_fail > 0) {
		fake_pub_fail--;
		fake_pub_calls++;
		return TS_E_IO;
	}
	strcpy(prev_key, last_key);
	memcpy(prev_payload, last_payload, sizeof(prev_payload));
	prev_len = last_len;
	strncpy(last_key, key, sizeof(last_key) - 1);
	last_key[sizeof(last_key) - 1] = '\0';
	memcpy(last_payload, p, l > sizeof(last_payload) ? sizeof(last_payload) : l);
	last_len = l;
	last_qos = qos;
	fake_pub_calls++;
	return TS_OK;
}
static bool fake_is_up(void)
{
	return fake_up_v;
}
static const ts_net_transport_t fake_t = {
	.open = fake_open, .close = fake_close,
	.publish = fake_publish, .is_up = fake_is_up,
};

/* ---- L1：keyspace -------------------------------------------------------- */

ZTEST(framework_net, test_01_keyspace)
{
	char buf[64];

	zassert_equal(ts_net_set_ids("n1", "c1"), TS_OK);
	zassert_equal(ts_net_set_ids(NULL, "c1"), TS_E_PARAM);
	zassert_equal(ts_net_set_ids("n1", ""), TS_E_PARAM);
	zassert_equal(ts_net_set_ids("overlong-node-id-0123456789abcdef0123456789", "c"),
		      TS_E_PARAM, "前缀超 48B 拒绝");

	zassert_true(ts_net_key_cmd(buf, sizeof(buf), "led1") > 0);
	zassert_equal(strcmp(buf, "tessera/n1/c1/led1/cmd"), 0);
	zassert_true(ts_net_key_tel(buf, sizeof(buf), "led1") > 0);
	zassert_equal(strcmp(buf, "tessera/n1/c1/led1/telemetry"), 0);
	zassert_true(ts_net_key_evt(buf, sizeof(buf), "led1") > 0);
	zassert_equal(strcmp(buf, "tessera/n1/c1/led1/event"), 0);
	zassert_true(ts_net_key_hb(buf, sizeof(buf), false) > 0);
	zassert_equal(strcmp(buf, "tessera/n1/c1/sys/hb"), 0, "cube→host（DR-12）");
	zassert_true(ts_net_key_hb(buf, sizeof(buf), true) > 0);
	zassert_equal(strcmp(buf, "tessera/n1/c1/sys/hb-host"), 0, "host→cube（DR-12）");
	zassert_true(ts_net_key_sys(buf, sizeof(buf), "get-info") > 0);
	zassert_equal(strcmp(buf, "tessera/n1/c1/sys/get-info"), 0);

	/* 截断语义：n 不足返回需要长度且不越界写 */
	char small[8] = "ZZZZZZZZ";
	int need = ts_net_key_cmd(small, 8, "led1");

	zassert_true(need > (int)sizeof(small), "返回所需长度");
	zassert_equal(small[7], '\0', "写入以 NUL 截断，无越界");
	zassert_equal(ts_net_key_cmd(buf, sizeof(buf), NULL), -1);
	zassert_equal(ts_net_key_sys(buf, sizeof(buf), ""), -1);
}

/* ---- L1：退避表（DEC-27 固定表循环）-------------------------------------- */

ZTEST(framework_net, test_02_backoff_table)
{
	zassert_equal(ts_net_backoff_ms(0), 250);
	zassert_equal(ts_net_backoff_ms(1), 500);
	zassert_equal(ts_net_backoff_ms(2), 1000);
	zassert_equal(ts_net_backoff_ms(3), 2000);
	zassert_equal(ts_net_backoff_ms(4), 250, "回绕循环");
	zassert_equal(ts_net_backoff_ms(9), 500);
}

/* ---- L2：pubq 语义（DOWN 丢弃 / 满丢最旧 / flush）------------------------ */

ZTEST(framework_net, test_03_pubq)
{
	uint8_t p[4] = {1, 2, 3, 4};

	ts_net_test_reset();
	zassert_equal(ts_net_state(), TS_NET_DOWN);

	/* DOWN 期 = 直接丢弃并计数 */
	zassert_equal(ts_net_pubq_push("tessera/n1/c1/led1/telemetry", p, 4), TS_OK);
	zassert_equal(ts_net_pubq_dropped(), 1, "DOWN 期丢弃计数");

	/* 建链后 push→flush */
	fake_open_fail = 0;
	fake_up_v = true;
	fake_pub_calls = 0;
	ts_net_set_transport(&fake_t);
	zassert_equal(ts_net_session_poll(0), TS_NET_CONNECTED);
	for (int i = 0; i < 3; i++) {
		zassert_equal(ts_net_pubq_push("k/a", p, 4), TS_OK);
	}
	zassert_equal(ts_net_pubq_dropped(), 1);
	ts_net_pubq_flush();
	zassert_equal(fake_pub_calls, 3, "三条全部发出");
	zassert_equal(strcmp(last_key, "k/a"), 0);

	/* 满溢：push 11 条 → 8 入队 + 3 丢最旧 */
	for (int i = 0; i < 11; i++) {
		zassert_equal(ts_net_pubq_push("k/b", p, 4), TS_OK);
	}
	zassert_equal(ts_net_pubq_dropped(), 1 + 3, "溢出丢最旧计数");

	/* 参数域 */
	zassert_equal(ts_net_pubq_push(NULL, p, 4), TS_E_PARAM);
	zassert_equal(ts_net_pubq_push("k", NULL, 4), TS_E_PARAM);
	zassert_equal(ts_net_pubq_push("k", p, 0), TS_E_PARAM);
}

/* ---- L1/L2：会话状态机（建链/退避推进/掉线迁移）-------------------------- */

ZTEST(framework_net, test_04_session)
{
	ts_net_test_reset();
	fake_pub_calls = 0;
	fake_close_calls = 0;

	/* 无传输 = 不动 */
	zassert_equal(ts_net_session_poll(0), TS_NET_DOWN);

	/* open 失败 → 退避表推进（250/500）*/
	fake_open_fail = 1;
	fake_up_v = false;
	ts_net_set_transport(&fake_t);
	zassert_equal(ts_net_session_poll(0), TS_NET_DOWN);
	zassert_equal(ts_net_session_poll(100), TS_NET_DOWN, "退避期内不重试");
	zassert_equal(ts_net_session_poll(250), TS_NET_DOWN, "第二次失败");
	zassert_equal(ts_net_session_poll(300), TS_NET_DOWN, "500ms 退避期内不重试");
	zassert_equal(ts_net_session_poll(750), TS_NET_DOWN, "第三次失败");

	/* open 成功 → CONNECTED */
	fake_open_fail = 0;
	fake_up_v = true;
	zassert_equal(ts_net_session_poll(1750), TS_NET_CONNECTED, "建链");

	/* 传输掉线 → DOWN + close + 从表头退避 */
	fake_up_v = false;
	zassert_equal(ts_net_session_poll(1800), TS_NET_DOWN, "掉线迁移");
	zassert_equal(fake_close_calls, 1);
	zassert_equal(ts_net_session_poll(2000), TS_NET_DOWN, "250ms 退避");
	zassert_equal(ts_net_session_poll(2100), TS_NET_DOWN);
}

/* ---- L2 集成：linkmon → ts-safety（合同 3 判定源）------------------------ */

ZTEST(framework_net, test_05_linkmon_safety)
{
	static const ts_out_ch_t led_ch = {
		.uid = "netled", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = true}, .fault = {.b = false},
	};
	ts_out_value_t rb;

	ts_net_test_reset();
	zassert_equal(ts_safety_register_channel(&led_ch), TS_OK);
	zassert_false(ts_net_link_up(), "初始判定 = down");

	/* 判定 down 期：commit 被安全层拒绝（链路未建立） */
	zassert_equal(ts_safety_commit("netled", (ts_out_value_t){.b = true}), TS_E_STATE);

	/* 恢复滞回：1 次心跳不足，2 次（DEC-27）→ up */
	ts_net_linkmon_hb_host(100);
	zassert_false(ts_net_link_up(), "滞回：单次不足");
	ts_net_linkmon_hb_host(1100);
	zassert_true(ts_net_link_up());
	zassert_equal(ts_safety_commit("netled", (ts_out_value_t){.b = true}), TS_OK);

	/* 断链：6 个周期（DEC-22）+ ε 未达 → set_link(false) → LINKLOSS 态值 */
	ts_net_linkmon_tick(1100 + 6 * 1000 + 1);
	zassert_false(ts_net_link_up(), "超时判定断链");
	ts_ch_state_t st;

	zassert_equal(ts_safety_channel_state("netled", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_LINKLOSS, "通道进断链安全态");
	zassert_equal(ts_safety_readback("netled", &rb), TS_OK);
	zassert_true(rb.b, "linkloss 态值生效（.b=true）");

	/* 恢复：断链后心跳恢复 → 连续 2 次重新 up（streak 已被断链清零） */
	ts_net_linkmon_hb_host(7300);
	zassert_false(ts_net_link_up());
	ts_net_linkmon_hb_host(7400);
	zassert_true(ts_net_link_up(), "滞回恢复");
}

/* ---- M3a.2：sys 命令面矩阵（LLD-ts-net §4，DEC-30①）---------------------- */

static uint8_t resp[512];
static size_t resp_len;

/* 构造请求 map{"op": op} / 带参 map{"op": op, "args": {...}}（定体 canonical） */
static size_t build_req(uint8_t *buf, size_t cap, const char *op,
			const char *arg_key, const char *arg_str, uint64_t arg_uint)
{
	size_t p = 0;

	if (arg_key == NULL) {
		zassert_true(ts_cbor_put_map(buf, cap, &p, 1));
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "op"));
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, op));
		return p;
	}
	zassert_true(ts_cbor_put_map(buf, cap, &p, 2));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "op"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, op));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "args"));
	zassert_true(ts_cbor_put_map(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, arg_key));
	if (arg_str != NULL) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, arg_str));
	} else {
		zassert_true(ts_cbor_put_uint(buf, cap, &p, arg_uint));
	}
	return p;
}

/* 解回执首对（"status"）验证编码合法性并取值 */
static int64_t resp_status(void)
{
	ts_cbor_rd_t r;
	uint32_t pairs;

	ts_cbor_rd_init(&r, resp, resp_len);
	zassert_true(ts_cbor_map_open(&r, &pairs));
	char key[8];

	zassert_true(ts_cbor_tstr(&r, key, sizeof(key)));
	zassert_equal(strcmp(key, "status"), 0);
	int64_t v;

	zassert_true(ts_cbor_int(&r, &v));
	return v;
}

ZTEST(framework_net, test_06_syscmd_matrix)
{
	uint8_t req[128];
	size_t rlen;

	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_cmd_sys_init();
	zassert_equal(ts_net_set_ids("n1", "c1"), TS_OK);

	/* get-info / get-link / get-safety / get-audit → OK 且回执合法 CBOR */
	const char *oks[] = {"get-info", "get-link", "get-safety", "get-audit"};

	for (size_t i = 0; i < 4; i++) {
		char key[32];

		snprintf(key, sizeof(key), "sys/%s", oks[i]);
		rlen = build_req(req, sizeof(req), oks[i], NULL, NULL, 0);
		zassert_equal(ts_net_cmd_dispatch(key, req, rlen, resp, sizeof(resp),
						  &resp_len),
			      TS_OK, "cmd %s", oks[i]);
		zassert_equal(resp_status(), TS_OK);
		zassert_true(resp_len > 10, "回执含 data");
	}

	/* get-budget：M3b 实装 → OK（预算快照字段；prov 未载 → budget=0 安全侧） */
	rlen = build_req(req, sizeof(req), "get-budget", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/get-budget", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(resp_status(), TS_OK);

	/* set-time：无 time_ms → PARAM；带 time_ms → OK + 墙钟生效（DR-08） */
	rlen = build_req(req, sizeof(req), "set-time", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/set-time", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_PARAM);
	zassert_equal(resp_status(), TS_E_PARAM);
	rlen = build_req(req, sizeof(req), "set-time", "time_ms", NULL, 1700000000000ULL);
	zassert_equal(ts_net_cmd_dispatch("sys/set-time", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(ts_time_wall_ms(), 1700000000000ULL, "墙钟数据字段生效");

	/* estop-clear：令牌缺失/错误 → PARAM；正确令牌 → 走 clear_fault 路径 */
	rlen = build_req(req, sizeof(req), "estop-clear", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/estop-clear", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_PARAM);
	rlen = build_req(req, sizeof(req), "estop-clear", "confirm", "stop", 0);
	zassert_equal(ts_net_cmd_dispatch("sys/estop-clear", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_PARAM, "错误令牌拒绝");
	rlen = build_req(req, sizeof(req), "estop-clear", "confirm", "estop", 0);
	(void)ts_net_cmd_dispatch("sys/estop-clear", req, rlen, resp, sizeof(resp),
				  &resp_len);
	/* 无锁存时 clear_fault 语义由 safety 定义——此处只断回执编码合法 */

	/* 未知 key / op 与 key 不匹配 / 垃圾请求 → NOTFOUND/PARAM（不留静默） */
	rlen = build_req(req, sizeof(req), "get-info", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/bogus", req, rlen, resp, sizeof(resp),
					  &resp_len),
		      TS_E_NOTFOUND, "未知 key");
	zassert_equal(ts_net_cmd_dispatch("sys/get-link", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_NOTFOUND, "op/key 不匹配");
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, 1, resp,
					  sizeof(resp), &resp_len),
		      TS_E_PARAM, "垃圾请求");
	static const uint8_t garbage[4] = {0xA2, 0x63, 0x66, 0x6F};

	zassert_equal(ts_net_cmd_dispatch("sys/get-info", garbage, sizeof(garbage),
					  resp, sizeof(resp), &resp_len),
		      TS_E_PARAM, "非法 CBOR");
}

/* ---- M3a.2：遥测快照 + 事件外发 → pubq → transport ----------------------- */

ZTEST(framework_net, test_07_telemetry_and_events)
{
	static const ts_out_ch_t led_ch = {
		.uid = "tel1", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
	};
	static const ts_hal_dev_desc_t led_dev = {.uid = "tel1", .kind = TS_DEV_GPIO_OUT};

	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_cmd_sys_init();
	(void)ts_net_pub_init(); /* 幂等：重复订阅由容量守卫拒绝 */
	zassert_equal(ts_safety_register_channel(&led_ch), TS_OK);
	zassert_equal(ts_hal_register_dev(&led_dev), TS_OK);

	/* DOWN 期：快照入 pubq = 丢弃计数（输出快照 + 预算记录两条，M3b） */
	uint32_t d0 = ts_net_pubq_dropped();

	ts_net_pub_telem(0);
	zassert_equal(ts_net_pubq_dropped(), d0 + 2, "DOWN 期丢弃 = 输出快照 + 预算记录");

	/* 建链后：快照 → flush → fake 捕获 telemetry key + value_u */
	fake_open_fail = 0;
	fake_up_v = true;
	fake_pub_calls = 0;
	ts_net_set_transport(&fake_t);
	zassert_equal(ts_net_session_poll(0), TS_NET_CONNECTED);
	ts_net_pub_telem(10);
	zassert_equal(ts_net_pubq_dropped(), d0 + 2);
	ts_net_pubq_flush();
	zassert_equal(fake_pub_calls, 2, "输出快照 + 预算记录两条");
	zassert_equal(strcmp(prev_key, "tessera/n1/c1/tel1/telemetry"), 0,
		      "实例遥测 key（前一条）");
	zassert_equal(strcmp(last_key, "tessera/n1/c1/sys/power"), 0,
		      "预算记录 key（M3b kind 97）");
	zassert_true(prev_len > 20, "遥测 payload = 定体 CBOR 快照");

	/* 信封 v1（DEC-42）：map{ver:1, kind:96, dev, value_u, wall_ms} 全 uint 值 */
	{
		ts_cbor_rd_t r;
		uint32_t pairs;
		char k[8];
		uint64_t ver = 0, kind = 0, dev = 0;

		ts_cbor_rd_init(&r, prev_payload, prev_len);
		zassert_true(ts_cbor_map_open(&r, &pairs));
		zassert_equal(pairs, 5);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "ver") == 0);
		zassert_true(ts_cbor_uint(&r, &ver) && ver == 1);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "kind") == 0);
		zassert_true(ts_cbor_uint(&r, &kind) && kind == 96, "遥测 kind=96（§4.4）");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "dev") == 0);
		zassert_true(ts_cbor_uint(&r, &dev) && dev == (uint64_t)TS_DEV_GPIO_OUT);
		zassert_equal(last_qos, TS_NET_QOS_BESTEFFORT, "遥测 = 丢弃式缺省");
	}

	/* 预算记录信封（kind 97，M3b）：ver/kind + budget_ma/used_ma/peak_ma/wall */
	{
		ts_cbor_rd_t r;
		uint32_t pairs;
		char k[12];
		uint64_t u;
		uint32_t kind = 0;

		ts_cbor_rd_init(&r, last_payload, last_len);
		zassert_true(ts_cbor_map_open(&r, &pairs));
		zassert_equal(pairs, 6);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "ver") == 0);
		zassert_true(ts_cbor_uint(&r, &u) && u == 1);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "kind") == 0);
		zassert_true(ts_cbor_uint(&r, &u));
		kind = (uint32_t)u;
		zassert_equal(kind, 97, "预算 kind=97（§4.4 遥测区间）");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "budget_ma") == 0);
		zassert_true(ts_cbor_uint(&r, &u));
		zassert_equal(u, 0, "prov 未载 = 预算 0（安全侧）");
	}

	/* 事件：SAFE_STATE_CHANGED（含 uid）→ event key 外发 */
	fake_pub_calls = 0;
	const ts_safe_state_evt_t pl = {.uid = "tel1", .new_state = TS_ST_ACTIVE};
	const ts_evt_t evt = {
		.id = TS_EVT_SAFE_STATE_CHANGED, .t_ms = 20,
		.data = &pl, .len = sizeof(pl),
	};

	ts_evt_publish(&evt);
	ts_net_pubq_flush();
	zassert_equal(fake_pub_calls, 1, "事件一条");
	zassert_equal(strcmp(last_key, "tessera/n1/c1/tel1/event"), 0, "实例事件路由");
	/* 信封（DEC-42）：kind = 32+evt_id；安全事件 = 阻塞式高优先级 QoS */
	{
		ts_cbor_rd_t r;
		uint32_t pairs;
		char k[8];
		uint64_t ver = 0, kind = 0;

		ts_cbor_rd_init(&r, last_payload, last_len);
		zassert_true(ts_cbor_map_open(&r, &pairs));
		zassert_equal(pairs, 5);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "ver") == 0);
		zassert_true(ts_cbor_uint(&r, &ver) && ver == 1);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "kind") == 0);
		zassert_true(ts_cbor_uint(&r, &kind));
		zassert_equal(kind, (uint64_t)(32 + (int)TS_EVT_SAFE_STATE_CHANGED),
			      "事件 kind = 32+evt_id（§4.4）");
		zassert_equal(last_qos, TS_NET_QOS_SAFETY, "安全事件 = 阻塞式高优先级");
	}

	/* F-1（docs/history/review-archive）：periph 插拔事件归因外发（LLD-ts-periph §3
	 * "key 下线通告"）——payload 携 uid/pkind，QoS = 尽力而为 */
	fake_pub_calls = 0;
	const ts_periph_evt_t ppl = {.uid = "tel1", .kind = (uint8_t)TS_PK_GPIO};
	const ts_evt_t pevt = {
		.id = TS_EVT_PERIPH_DETACH, .t_ms = 30,
		.data = &ppl, .len = sizeof(ppl),
	};

	ts_evt_publish(&pevt);
	ts_net_pubq_flush();
	zassert_equal(fake_pub_calls, 1, "插拔事件一条");
	zassert_equal(strcmp(last_key, "tessera/n1/c1/tel1/event"), 0, "按 uid 路由");
	{
		ts_cbor_rd_t r;
		uint32_t pairs;
		char k[8], uid[16];
		uint64_t ver = 0, kind = 0, pkind = 0;

		ts_cbor_rd_init(&r, last_payload, last_len);
		zassert_true(ts_cbor_map_open(&r, &pairs));
		zassert_equal(pairs, 6, "信封四对 + uid/pkind");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "ver") == 0);
		zassert_true(ts_cbor_uint(&r, &ver) && ver == 1);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "kind") == 0);
		zassert_true(ts_cbor_uint(&r, &kind));
		zassert_equal(kind, (uint64_t)(32 + (int)TS_EVT_PERIPH_DETACH),
			      "插拔 kind = 32+evt_id");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "t_ms") == 0);
		zassert_true(ts_cbor_uint(&r, &kind));
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "wall_ms") == 0);
		zassert_true(ts_cbor_uint(&r, &kind));
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "uid") == 0);
		zassert_true(ts_cbor_tstr(&r, uid, sizeof(uid)));
		zassert_equal(strcmp(uid, "tel1"), 0, "归因 uid");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "pkind") == 0);
		zassert_true(ts_cbor_uint(&r, &pkind));
		zassert_equal(pkind, (uint64_t)TS_PK_GPIO, "periph 类别");
		zassert_equal(last_qos, TS_NET_QOS_BESTEFFORT, "插拔 = 尽力而为（DEC-42 类属）");
	}

	/* F-1：供电预算拒绝事件外发——payload 携 requested/used/budget 三元组 */
	fake_pub_calls = 0;
	const ts_pwr_budget_evt_t bpl = {
		.requested_ma = 200, .used_ma = 150, .budget_ma = 300,
	};
	const ts_evt_t bevt = {
		.id = TS_EVT_POWER_BUDGET, .t_ms = 40,
		.data = &bpl, .len = sizeof(bpl),
	};

	ts_evt_publish(&bevt);
	ts_net_pubq_flush();
	zassert_equal(fake_pub_calls, 1, "预算事件一条");
	zassert_equal(strcmp(last_key, "tessera/n1/c1/sys/event"), 0, "系统事件路由");
	{
		ts_cbor_rd_t r;
		uint32_t pairs;
		char k[16];
		uint64_t u = 0, kind = 0;

		ts_cbor_rd_init(&r, last_payload, last_len);
		zassert_true(ts_cbor_map_open(&r, &pairs));
		zassert_equal(pairs, 7, "信封四对 + 三元组");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "ver") == 0);
		zassert_true(ts_cbor_uint(&r, &u) && u == 1);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "kind") == 0);
		zassert_true(ts_cbor_uint(&r, &kind));
		zassert_equal(kind, (uint64_t)(32 + (int)TS_EVT_POWER_BUDGET),
			      "预算事件 kind = 32+evt_id");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "t_ms") == 0);
		zassert_true(ts_cbor_uint(&r, &u));
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "wall_ms") == 0);
		zassert_true(ts_cbor_uint(&r, &u));
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "requested_ma") == 0);
		zassert_true(ts_cbor_uint(&r, &u) && u == 200, "请求值归因");
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "used_ma") == 0);
		zassert_true(ts_cbor_uint(&r, &u) && u == 150);
		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "budget_ma") == 0);
		zassert_true(ts_cbor_uint(&r, &u) && u == 300);
		zassert_equal(last_qos, TS_NET_QOS_BESTEFFORT, "预算事件 = 尽力而为");
	}
}

/* ---- DEC-40 批：命令信封 v2 + 幂等回执缓存 -------------------------------- */

/* v2 请求 map{ver:1, kind:1, rid, src:"t-agent", op, args?{arg1?, idem?, to?}} */
static size_t build_req_v2(uint8_t *buf, size_t cap, const char *op, const char *rid,
			   const char *arg1k, const char *arg1s, uint64_t arg1u,
			   const char *idem, bool has_to, uint32_t to)
{
	bool has_args = arg1k != NULL || idem != NULL || has_to;
	uint32_t apairs = (arg1k ? 1u : 0u) + (idem ? 1u : 0u) + (has_to ? 1u : 0u);
	size_t p = 0;

	zassert_true(ts_cbor_put_map(buf, cap, &p, 5 + (has_args ? 1 : 0)));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "ver"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "kind"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "rid"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, rid));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "src"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "t-agent"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "op"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, op));
	if (has_args) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "args"));
		zassert_true(ts_cbor_put_map(buf, cap, &p, apairs));
		if (arg1k != NULL) {
			zassert_true(ts_cbor_put_tstr(buf, cap, &p, arg1k));
			if (arg1s != NULL) {
				zassert_true(ts_cbor_put_tstr(buf, cap, &p, arg1s));
			} else {
				zassert_true(ts_cbor_put_uint(buf, cap, &p, arg1u));
			}
		}
		if (idem != NULL) {
			zassert_true(ts_cbor_put_tstr(buf, cap, &p, "idem"));
			zassert_true(ts_cbor_put_tstr(buf, cap, &p, idem));
		}
		if (has_to) {
			zassert_true(ts_cbor_put_tstr(buf, cap, &p, "to"));
			zassert_true(ts_cbor_put_uint(buf, cap, &p, to));
		}
	}
	return p;
}

/* 解 v2 回执 map{ver,kind,rid,status,data}：验证信封头 + rid 回带，取 status */
static int64_t resp2_status(const char *expect_rid)
{
	ts_cbor_rd_t r;
	uint32_t pairs;
	char k[8], rid[24];
	uint64_t u;
	int64_t st;

	ts_cbor_rd_init(&r, resp, resp_len);
	zassert_true(ts_cbor_map_open(&r, &pairs));
	zassert_equal(pairs, 5, "v2 回执五对");
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "ver") == 0);
	zassert_true(ts_cbor_uint(&r, &u) && u == 1);
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "kind") == 0);
	zassert_true(ts_cbor_uint(&r, &u) && u == 16, "回执 kind=16（§4.4）");
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "rid") == 0);
	zassert_true(ts_cbor_tstr(&r, rid, sizeof(rid)));
	zassert_equal(strcmp(rid, expect_rid), 0, "rid 原样回带");
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "status") == 0);
	zassert_true(ts_cbor_int(&r, &st));
	return st;
}

ZTEST(framework_net, test_08_env_v2_idem)
{
	uint8_t req[192];
	uint8_t first[512];
	size_t flen;
	size_t rlen;

	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_cmd_sys_init();

	/* v2 基础：rid 回带 + kind 16 + status 语义 */
	rlen = build_req_v2(req, sizeof(req), "get-info", "rid-1", NULL, NULL, 0,
			    NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_OK);
	zassert_equal(resp2_status("rid-1"), TS_OK);

	/* v1 共存（首键判别）：v1 回执仍两对 {status,data}——弃用期不破坏 */
	rlen = build_req(req, sizeof(req), "get-info", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_OK);
	zassert_equal(resp_status(), TS_OK);

	/* v2 fail-closed：缺 src / kind!=1 / 未知信封键 = PARAM */
	size_t p = 0;

	zassert_true(ts_cbor_put_map(req, sizeof(req), &p, 4));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "ver"));
	zassert_true(ts_cbor_put_uint(req, sizeof(req), &p, 1));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "kind"));
	zassert_true(ts_cbor_put_uint(req, sizeof(req), &p, 1));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "rid"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "r"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "op"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "get-info"));
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, p, resp, sizeof(resp),
					  &resp_len), TS_E_PARAM, "缺 src 拒绝");

	rlen = build_req_v2(req, sizeof(req), "get-info", "rid-k", NULL, NULL, 0,
			    NULL, false, 0);
	/* 手改 kind 值（map hdr 1B + "ver" 4B + 1B + "kind" 5B → 值偏移 = 11） */
	req[11] = 2; /* kind=2（未注册的请求 kind） */
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_E_PARAM, "kind!=1 拒绝");
	req[11] = 1; /* 还原 */

	p = 0;
	zassert_true(ts_cbor_put_map(req, sizeof(req), &p, 6));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "ver"));
	zassert_true(ts_cbor_put_uint(req, sizeof(req), &p, 1));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "kind"));
	zassert_true(ts_cbor_put_uint(req, sizeof(req), &p, 1));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "rid"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "r"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "src"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "t"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "op"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "get-info"));
	zassert_true(ts_cbor_put_tstr(req, sizeof(req), &p, "bogus"));
	zassert_true(ts_cbor_put_uint(req, sizeof(req), &p, 1));
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, p, resp, sizeof(resp),
					  &resp_len), TS_E_PARAM, "未知信封键拒绝");

	/* to 边界（DEC-40）：5000 OK；5001 拒 */
	rlen = build_req_v2(req, sizeof(req), "get-info", "rid-t", NULL, NULL, 0,
			    NULL, true, 5000);
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_OK, "to=5000 上界内");
	rlen = build_req_v2(req, sizeof(req), "get-info", "rid-t", NULL, NULL, 0,
			    NULL, true, 5001);
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_E_PARAM, "to>5000 拒绝");
	zassert_equal(resp2_status("rid-t"), TS_E_PARAM, "拒绝回执仍是 v2 信封");

	/* 幂等（DEC-40）：同 idem 重发 = 原样回执，不重执行 */
	rlen = build_req_v2(req, sizeof(req), "set-time", "rid-a", "time_ms", NULL,
			    111111ULL, "idem-a", false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/set-time", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_OK);
	memcpy(first, resp, resp_len);
	flen = resp_len;
	zassert_equal(ts_time_wall_ms(), 111111ULL, "首次执行生效");
	rlen = build_req_v2(req, sizeof(req), "set-time", "rid-b", "time_ms", NULL,
			    222222ULL, "idem-a", false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/set-time", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_OK);
	zassert_equal(resp_len, flen);
	zassert_equal(memcmp(first, resp, flen), 0, "回放 = 字节级原样回执");
	zassert_equal(ts_time_wall_ms(), 111111ULL, "未重执行（墙钟不变）");

	/* idem 与 op 不匹配（调用方键管理错误）→ PARAM */
	rlen = build_req_v2(req, sizeof(req), "get-info", "rid-c", NULL, NULL, 0,
			    "idem-a", false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_E_PARAM, "idem/op mismatch");

	/* LRU 驱逐（深度 CONFIG_TS_NET_IDEM_CACHE=4）：填 b/c/d/e → a 被驱逐重执行 */
	const char *fill[] = {"idem-b", "idem-c", "idem-d"};

	for (int i = 0; i < 3; i++) {
		rlen = build_req_v2(req, sizeof(req), "set-time", "rid-f", "time_ms",
				    NULL, 500 + (uint64_t)i, fill[i], false, 0);
		(void)ts_net_cmd_dispatch("sys/set-time", req, rlen, resp, sizeof(resp),
					  &resp_len);
	}
	rlen = build_req_v2(req, sizeof(req), "set-time", "rid-f", "time_ms", NULL,
			    999ULL, "idem-e", false, 0);
	(void)ts_net_cmd_dispatch("sys/set-time", req, rlen, resp, sizeof(resp),
				  &resp_len); /* 第 5 项 → 驱逐最旧 idem-a */
	rlen = build_req_v2(req, sizeof(req), "set-time", "rid-g", "time_ms", NULL,
			    333333ULL, "idem-a", false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/set-time", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_OK);
	zassert_equal(ts_time_wall_ms(), 333333ULL, "驱逐后同 idem = 重新执行");
}

/* ---- DEC-41 批：控制租约生命周期 ------------------------------------------ */

ZTEST(framework_net, test_09_lease)
{
	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_cmd_sys_init();
	ts_net_lease_test_reset();

	const uint32_t ttl = CONFIG_TS_NET_LEASE_TTL_MS;
	uint32_t id = 0;
	uint64_t exp = 0;
	bool valid = false;
	char holder[TS_NET_LEASE_HOLDER_MAX];
	uint32_t gid = 0;
	uint64_t gexp = 0;

	/* 空态：get → invalid；release 幂等 OK */
	ts_net_lease_get(0, &valid, holder, sizeof(holder), &gid, &gexp);
	zassert_false(valid);
	zassert_equal(gid, 0);
	zassert_equal(ts_net_lease_release("a", 0), TS_OK, "无租约释放 = 幂等空操作");

	/* 获取：id 从 1 起，TTL 自 now 顺延 */
	zassert_equal(ts_net_lease_acquire("agent-a", 1000, &id, &exp), TS_OK);
	zassert_equal(id, 1);
	zassert_equal(exp, 1000 + ttl);
	zassert_true(ts_net_lease_held_by("agent-a", 1500));
	zassert_false(ts_net_lease_held_by("agent-b", 1500));

	/* 续期幂等：同 holder → id 不变，窗口自 now 顺延 */
	zassert_equal(ts_net_lease_acquire("agent-a", 5000, &id, &exp), TS_OK);
	zassert_equal(id, 1, "续期不换 id");
	zassert_equal(exp, 5000 + ttl);

	/* 他人获取 → STATE（回填当前租约） */
	zassert_equal(ts_net_lease_acquire("agent-b", 6000, &id, &exp), TS_E_STATE);
	zassert_equal(id, 1);
	zassert_equal(exp, 5000 + ttl, "拒绝回执回填当前租约");

	/* 惰性过期：now == expires 即失效；过期后再获取 → 新 id（不复用） */
	zassert_false(ts_net_lease_held_by("agent-a", 5000 + ttl));
	zassert_equal(ts_net_lease_acquire("agent-b", 5000 + ttl + 1, &id, &exp), TS_OK);
	zassert_equal(id, 2, "过期后新授予 id 递增");

	/* 代还拒绝；本人归还 OK；重复归还幂等 */
	zassert_equal(ts_net_lease_release("agent-a", 5000 + ttl + 2), TS_E_STATE);
	zassert_equal(ts_net_lease_release("agent-b", 5000 + ttl + 3), TS_OK);
	zassert_equal(ts_net_lease_release("agent-b", 5000 + ttl + 4), TS_OK);
	ts_net_lease_get(5000 + ttl + 5, &valid, holder, sizeof(holder), &gid, &gexp);
	zassert_false(valid);

	/* 参数域：空/超长 holder 拒绝 */
	zassert_equal(ts_net_lease_acquire(NULL, 0, NULL, NULL), TS_E_PARAM);
	zassert_equal(ts_net_lease_acquire("", 0, NULL, NULL), TS_E_PARAM);
	zassert_equal(ts_net_lease_acquire("holder-of-24-chars-xxxxx", 0, NULL, NULL),
		      TS_E_PARAM, "超长 holder 拒绝");

	/* 命令面全链（v2 信封）：acquire → get → 他人 release 拒 → 本人 release */
	uint8_t req[192];
	size_t rlen = build_req_v2(req, sizeof(req), "lease-acquire", "rid-l1",
				   "holder", "panel-1", 0, NULL, false, 0);

	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &resp_len), TS_OK);
	zassert_equal(resp2_status("rid-l1"), TS_OK);

	rlen = build_req_v2(req, sizeof(req), "lease-get", "rid-l2", NULL, NULL, 0,
			    NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-get", req, rlen, resp, sizeof(resp),
					  &resp_len), TS_OK);
	zassert_equal(resp2_status("rid-l2"), TS_OK);

	rlen = build_req_v2(req, sizeof(req), "lease-release", "rid-l3",
			   "holder", "someone-else", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-release", req, rlen, resp,
					  sizeof(resp), &resp_len), TS_E_STATE,
		      "他人代还拒绝");
	rlen = build_req_v2(req, sizeof(req), "lease-release", "rid-l4",
			   "holder", "panel-1", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-release", req, rlen, resp,
					  sizeof(resp), &resp_len), TS_OK);

	/* lease-acquire 缺 holder → PARAM（fail-closed） */
	rlen = build_req_v2(req, sizeof(req), "lease-acquire", "rid-l5", NULL, NULL, 0,
			    NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &resp_len), TS_E_PARAM);
}

/* ---- MA3.1：远程部署命令面（LLD-agent-deploy-net §3；gated = v2 + 租约）---------------- */

static bool put_bstr(uint8_t *buf, size_t cap, size_t *pos, const uint8_t *d, uint32_t n)
{
	if (n < 24) {
		if (*pos + 1 > cap) return false;
		buf[(*pos)++] = 0x40 | (uint8_t)n;
	} else if (n < 256) {
		if (*pos + 2 > cap) return false;
		buf[(*pos)++] = 0x58;
		buf[(*pos)++] = (uint8_t)n;
	} else {
		if (*pos + 3 > cap) return false;
		buf[(*pos)++] = 0x59;
		buf[(*pos)++] = (uint8_t)(n >> 8);
		buf[(*pos)++] = (uint8_t)n;
	}
	if (*pos + n > cap) return false;
	memcpy(buf + *pos, d, n);
	*pos += n;
	return true;
}

/* v2 部署请求：args = {total} 或 {offset, data(bstr)}，src 可指定（门控测试） */
static size_t build_req_deploy(uint8_t *buf, size_t cap, const char *op, const char *src,
			       bool with_total, uint32_t total,
			       bool with_chunk, uint32_t offset, const uint8_t *data, uint32_t dlen)
{
	uint32_t apairs = (with_total ? 1u : 0u) + (with_chunk ? 2u : 0u);
	size_t p = 0;

	zassert_true(ts_cbor_put_map(buf, cap, &p, 6));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "ver"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "kind"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "rid"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "r-dep"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "src"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, src));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "op"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, op));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "args"));
	zassert_true(ts_cbor_put_map(buf, cap, &p, apairs));
	if (with_total) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "total"));
		zassert_true(ts_cbor_put_uint(buf, cap, &p, total));
	}
	if (with_chunk) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "offset"));
		zassert_true(ts_cbor_put_uint(buf, cap, &p, offset));
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "data"));
		zassert_true(put_bstr(buf, cap, &p, data, dlen));
	}
	return p;
}

/* 预签 v2 夹具（与 framework.appmgr 同源；DEC-49④ 真验签语义） */
#include "../../appmgr/src/fixture_pkg.h"

static size_t build_deploy_pkg(uint8_t *buf, size_t cap)
{
	if (sizeof(fixture_pkg) > cap) {
		return 0;
	}
	memcpy(buf, fixture_pkg, sizeof(fixture_pkg));
	return sizeof(fixture_pkg);
}

/* 解回执 data 子图中的 uint 值（v1 平面 / v2 信封均可；值按类型消费跳过） */
static uint64_t resp_data_uint(const char *key)
{
	ts_cbor_rd_t r;
	uint32_t pairs;

	ts_cbor_rd_init(&r, resp, resp_len);
	zassert_true(ts_cbor_map_open(&r, &pairs));
	/* 顶层定位 "data" 子图：v2 = {ver,kind,rid,status,data}；v1 = {status,data} */
	uint32_t dpairs = 0;
	bool in_data = false;

	for (uint32_t i = 0; i < pairs; i++) {
		char k[8];

		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)));
		if (strcmp(k, "data") == 0) {
			zassert_true(ts_cbor_map_open(&r, &dpairs));
			in_data = true;
			break;
		}
		if (strcmp(k, "rid") == 0) {
			char rid[24];

			zassert_true(ts_cbor_tstr(&r, rid, sizeof(rid)));
		} else {
			int64_t iv;

			zassert_true(ts_cbor_int(&r, &iv)); /* ver/kind/status */
		}
	}
	zassert_true(in_data, "回执无 data 子图");
	for (uint32_t i = 0; i < dpairs; i++) {
		char k[16];
		uint64_t v;

		zassert_true(ts_cbor_tstr(&r, k, sizeof(k)));
		if (strcmp(k, key) == 0 && ts_cbor_uint(&r, &v)) {
			return v;
		}
		/* 不匹配：按实际类型消费值（data 图值域 = uint/tstr） */
		if (!ts_cbor_uint(&r, &v)) {
			char dummy[24];

			zassert_true(ts_cbor_tstr(&r, dummy, sizeof(dummy)),
				     "未知值类型（本助手仅覆盖 uint/tstr）");
		}
	}
	return UINT64_MAX;
}

ZTEST(framework_net, test_10_deploy_face)
{
	static uint8_t pkg[1024];
	static uint8_t req[CONFIG_TS_NET_APP_CHUNK_MAX + 256];
	size_t plen = build_deploy_pkg(pkg, sizeof(pkg));
	size_t rlen;

	zassert_true(plen > 0, "pkg built");
	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_cmd_sys_init();
	ts_net_lease_test_reset();
	(void)ts_net_set_ids("n1", "c1");
	ts_store_test_reset();
	ts_appmgr_test_reset();

	/* v1 请求 → 拒（gated 面无 src 身份） */
	rlen = build_req(req, sizeof(req), "app-begin", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-begin", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_PARAM, "v1 拒绝（v2 required）");

	/* v2 无租约 → STATE（lease required） */
	rlen = build_req_deploy(req, sizeof(req), "app-begin", "t-agent",
				true, (uint32_t)plen, false, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-begin", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_STATE, "无租约拒绝");

	/* 他人持租约 → 仍拒（src ≠ holder） */
	uint32_t lid = 0;
	uint64_t lexp = 0;

	zassert_equal(ts_net_lease_acquire("other-owner", ts_time_ms(), &lid, &lexp),
		      TS_OK);
	zassert_equal(ts_net_cmd_dispatch("sys/app-begin", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_STATE, "src != holder 拒绝");

	/* 本人持租约 → 全链：begin → chunk×2 → verify → activate → get-app */
	zassert_equal(ts_net_lease_release("other-owner", ts_time_ms()), TS_OK);
	zassert_equal(ts_net_lease_acquire("t-agent", ts_time_ms(), &lid, &lexp), TS_OK);
	zassert_equal(ts_net_cmd_dispatch("sys/app-begin", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(resp_data_uint("slot"), 1, "inactive = B");
	zassert_equal(resp_data_uint("total"), (uint64_t)plen);

	/* 分块（两半）+ 断点续传进度 */
	uint32_t half = (uint32_t)plen / 2;

	rlen = build_req_deploy(req, sizeof(req), "app-chunk", "t-agent",
				false, 0, true, 0, pkg, half);
	zassert_equal(ts_net_cmd_dispatch("sys/app-chunk", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(resp_data_uint("high_water"), half, "进度 = 半包");
	rlen = build_req_deploy(req, sizeof(req), "app-chunk", "t-agent",
				false, 0, true, half, pkg + half, (uint32_t)plen - half);
	zassert_equal(ts_net_cmd_dispatch("sys/app-chunk", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(resp_data_uint("high_water"), (uint64_t)plen, "进度 = 全包");

	/* 越界块 → PARAM */
	rlen = build_req_deploy(req, sizeof(req), "app-chunk", "t-agent",
				false, 0, true, (uint32_t)plen, pkg, 4);
	zassert_equal(ts_net_cmd_dispatch("sys/app-chunk", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_PARAM);

	/* verify：容器事实（真验签通过；ml/wl 自夹具头动态断言） */
	rlen = build_req_deploy(req, sizeof(req), "app-verify", "t-agent",
				false, 0, false, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-verify", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(resp_data_uint("manifest_len"),
		      (((uint32_t)fixture_pkg[6] << 24) |
		       ((uint32_t)fixture_pkg[7] << 16) |
		       ((uint32_t)fixture_pkg[8] << 8) | fixture_pkg[9]));
	zassert_equal(resp_data_uint("cose_off"),
		      48 + (((uint32_t)fixture_pkg[6] << 24) |
			    ((uint32_t)fixture_pkg[7] << 16) |
			    ((uint32_t)fixture_pkg[8] << 8) | fixture_pkg[9]) +
		      (((uint32_t)fixture_pkg[10] << 24) |
		       ((uint32_t)fixture_pkg[11] << 16) |
		       ((uint32_t)fixture_pkg[12] << 8) | fixture_pkg[13]));

	/* activate（他人 src → 拒；本人 → OK） */
	rlen = build_req_deploy(req, sizeof(req), "app-activate", "intruder",
				false, 0, false, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-activate", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_STATE);
	rlen = build_req_deploy(req, sizeof(req), "app-activate", "t-agent",
				false, 0, false, 0, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-activate", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(resp_data_uint("active_slot"), 1);

	/* get-app（只读豁免）：active_slot 已切换 */
	rlen = build_req(req, sizeof(req), "get-app", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/get-app", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK, "get-app 只读不门控");
	zassert_equal(resp_data_uint("active_slot"), 1);
}

/* ---- review-archive（impl-review-01）修复批：F-3 gated 按 suffix 回填 / F-4 idem 先匹配 ---- */

static ts_res_t cmd_probe(const ts_net_cmd_args_t *a, uint8_t *r, size_t cap, size_t *n)
{
	ARG_UNUSED(a);
	ARG_UNUSED(r);
	ARG_UNUSED(cap);
	*n = 0;
	return TS_OK;
}

ZTEST(framework_net, test_11_gated_backfill_idem_order)
{
	uint8_t req[192];
	size_t rlen;

	ts_net_test_reset();
	ts_net_cmd_test_reset();
	/* F-3 场景：sys_init 前预注册外部命令（表头偏移一位）。按下标回填的
	 * 旧实现此时会把 app-activate 的门控位错压到别处（app-activate 丢门控、
	 * lease-get 被误门控）——按 suffix 回填后两者恒正确 */
	zassert_equal(ts_net_cmd_register("sys/probe", cmd_probe), TS_OK);
	zassert_equal(ts_net_cmd_sys_init(), TS_OK);

	/* 外部命令不被误门控：v1 可执行 */
	rlen = build_req(req, sizeof(req), "probe", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/probe", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK, "外部命令不门控");

	/* 判别点（修复前失败）：lease-get 只读不被误门控——v1 可执行 */
	rlen = build_req(req, sizeof(req), "lease-get", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-get", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK, "只读命令不因表偏移被误门控");

	/* 判别点（修复前失败）：app-activate 门控不因表偏移丢失——v1 拒绝 */
	rlen = build_req(req, sizeof(req), "app-activate", NULL, NULL, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-activate", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_PARAM, "部署面门控不因表偏移丢失（v1 required）");

	/* F-4：idem 回放不得跨 key——同 idem+op 发到别的 key = op/key mismatch
	 *（修复前：回放先于 key/op 匹配，会原样回放 get-info 的回执） */
	rlen = build_req_v2(req, sizeof(req), "get-info", "r-k1", NULL, NULL, 0,
			    "idem-x", false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK);
	zassert_equal(ts_net_cmd_dispatch("sys/get-link", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_E_NOTFOUND, "同 idem 跨 key = 拒绝（先匹配后回放）");

	/* 同 key 同 idem 重发 = 原样回放（正路径不回归） */
	rlen = build_req_v2(req, sizeof(req), "get-info", "r-k2", NULL, NULL, 0,
			    "idem-x", false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/get-info", req, rlen, resp,
					  sizeof(resp), &resp_len),
		      TS_OK, "同 key 同 idem 正常回放");
}

/* ---- L5：av 分片通道（DEC-47①②，MD1.2g）---------------------------------- */

ZTEST(framework_net, test_12_avq)
{
	char key[64];

	ts_net_test_reset();
	fake_pub_calls = 0;
	fake_pub_fail = 0;
	ts_net_avq_reset();
	zassert_equal(ts_net_set_ids("n1", "c1"), TS_OK);

	/* key 构造：专用帧分片通道（DEC-47①；stream = 数值 app_id） */
	zassert_true(ts_net_key_av(key, sizeof(key), 1) > 0);
	zassert_equal(strcmp(key, "tessera/n1/c1/av/1/frame"), 0);

	/* 参数门：chunk ≤1KB（DEC-47②）/ 分片序号域 / NULL */
	const uint8_t chunk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	static uint8_t big[CONFIG_TS_NET_PUBLISH_MAX_BYTES + 1];

	memset(big, 1, sizeof(big));
	zassert_equal(ts_net_avq_push(1, 1, 0, 2, NULL, 4), TS_E_PARAM);
	zassert_equal(ts_net_avq_push(1, 1, 0, 2, big, sizeof(big)), TS_E_PARAM,
		      "chunk > 1KB = 拒绝（永不进碎片路径）");
	zassert_equal(ts_net_avq_push(1, 1, 2, 2, chunk, 8), TS_E_PARAM,
		      "cid >= n = 拒绝");
	zassert_equal(ts_net_avq_push(1, 1, 0, 0, chunk, 8), TS_E_PARAM,
		      "n = 0 = 拒绝");

	/* DOWN 期：flush 自弃 + 计数（pubq 同语义） */
	zassert_equal(ts_net_avq_push(1, 7, 0, 2, chunk, 8), TS_OK);
	zassert_equal(ts_net_avq_push(1, 7, 1, 2, chunk, 8), TS_OK);
	ts_net_avq_flush();
	zassert_equal(fake_pub_calls, 0, "DOWN 期不发送");
	zassert_equal(ts_net_avq_dropped(), 2, "DOWN 期自弃计数");

	/* CONNECTED：发送 + key + 信封逐字段解码（fid/cid/n/crc/d） */
	ts_net_avq_reset();
	fake_open_fail = 0;
	fake_up_v = true;
	ts_net_set_transport(&fake_t);
	zassert_equal(ts_net_session_poll(0), TS_NET_CONNECTED);
	zassert_equal(ts_net_avq_push(1, 9, 0, 3, chunk, 8), TS_OK);
	int before = fake_pub_calls;

	ts_net_avq_flush();
	zassert_equal(fake_pub_calls, before + 1, "CONNECTED 发送");
	zassert_equal(strcmp(last_key, "tessera/n1/c1/av/1/frame"), 0);

	ts_cbor_rd_t r;
	uint32_t pairs;
	char k[8];
	uint64_t v;
	const uint8_t *d;
	uint32_t dl;

	ts_cbor_rd_init(&r, last_payload, last_len);
	zassert_true(ts_cbor_map_open(&r, &pairs));
	zassert_equal(pairs, 5);
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "fid") == 0);
	zassert_true(ts_cbor_uint(&r, &v) && v == 9);
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "cid") == 0);
	zassert_true(ts_cbor_uint(&r, &v) && v == 0);
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "n") == 0);
	zassert_true(ts_cbor_uint(&r, &v) && v == 3);
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "crc") == 0);
	zassert_true(ts_cbor_uint(&r, &v) && v == crc32_ieee(chunk, 8));
	zassert_true(ts_cbor_tstr(&r, k, sizeof(k)) && strcmp(k, "d") == 0);
	zassert_true(ts_cbor_bstr_ref(&r, &d, &dl) && dl == 8 &&
		     memcmp(d, chunk, 8) == 0);

	/* 队满背压：DEPTH 条入队后 E_BUSY（APP 稍后重发该分片） */
	ts_net_avq_reset();
	for (int i = 0; i < CONFIG_TS_NET_AVQ_DEPTH; i++) {
		zassert_equal(ts_net_avq_push(1, 10, i, CONFIG_TS_NET_AVQ_DEPTH,
					      chunk, 8), TS_OK);
	}
	zassert_equal(ts_net_avq_push(1, 10, 9, 10, chunk, 8), TS_E_BUSY,
		      "队满 = 背压");

	/* 重试路径：首次失败 → 重试成功送达（≤5×20ms——DEC-47②） */
	ts_net_avq_reset();
	fake_pub_fail = 1;
	zassert_equal(ts_net_avq_push(1, 11, 0, 1, chunk, 8), TS_OK);
	before = fake_pub_calls;
	ts_net_avq_flush();
	zassert_equal(fake_pub_calls, before + 2, "一次失败 + 重试成功");
	zassert_equal(ts_net_avq_dropped(), 0, "重试成功不计丢弃");
	fake_pub_fail = 0;
}

/* V0.4.0 M10（Q-34）：prov-write 令牌门（正路径归 framework.store 维护通道测试） */

ZTEST(framework_net, test_13_prov_write_gate)
{
	uint8_t req[192];
	uint8_t resp[192];
	size_t rlen;
	size_t rl2 = 0;

	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_lease_test_reset(); /* 执行序自足（前序用例租约残留） */
	zassert_equal(ts_net_cmd_sys_init(), TS_OK);

	/* gated 命令：先取租约（holder = 信封 src "t-agent"） */
	rlen = build_req_v2(req, sizeof(req), "lease-acquire", "rid-pw0",
			    "holder", "t-agent", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);

	/* 错误 confirm → E_PARAM（need 提示） */
	rlen = build_req_v2(req, sizeof(req), "prov-write", "rid-pw1",
			    "confirm", "wrong", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/prov-write", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_PARAM);

	/* 正确 confirm 但缺 data → E_PARAM（need data） */
	rlen = build_req_v2(req, sizeof(req), "prov-write", "rid-pw2",
			    "confirm", "prov", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/prov-write", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_PARAM);
}

/* ---- CG2（Q-41①③/Q-42）：部署模式守卫 + estop 远程清除配置面 ------------- */

/* prov v2 构造（store 测试同键序；走查正确性归 framework.store——此处仅
 * 供 net 侧行为测试注入模式/清除字段） */
static size_t build_prov_v2(uint8_t *buf, size_t cap, int ver, uint8_t deploy_mode,
			    uint8_t estop_remote_clear)
{
	static const uint8_t pk[32]; /* 全零（域值面归 store 测试） */
	size_t p = 0;

	zassert_true(ts_cbor_put_map(buf, cap, &p, ver == 2 ? 11 : 9));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "v"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, ver));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "node_id"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "nn"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "cube_id"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "cc"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "routers"));
	zassert_true(ts_cbor_put_array(buf, cap, &p, 1));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "tcp/1.2.3.4:9955"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "pk0"));
	zassert_true(ts_cbor_put_bstr(buf, cap, &p, pk, sizeof(pk)));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "pk1"));
	zassert_true(ts_cbor_put_bstr(buf, cap, &p, pk, sizeof(pk)));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "cred"));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, ""));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "pwr_ma"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 100));
	zassert_true(ts_cbor_put_tstr(buf, cap, &p, "estop"));
	zassert_true(ts_cbor_put_uint(buf, cap, &p, 0));
	if (ver == 2) {
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "deploy_mode"));
		zassert_true(ts_cbor_put_uint(buf, cap, &p, deploy_mode));
		zassert_true(ts_cbor_put_tstr(buf, cap, &p, "estop_remote_clear"));
		zassert_true(ts_cbor_put_uint(buf, cap, &p, estop_remote_clear));
	}
	return p;
}

/* LLD-ts-net §6 承诺用例：standalone 构建断链 = 状态零迁移断言 */
ZTEST(framework_net, test_14_standalone_zero_migration)
{
	static const ts_out_ch_t led_ch = {
		.uid = "saled", .kind = TS_CH_GPIO,
		.poweron = {.b = false}, .linkloss = {.b = true}, .fault = {.b = false},
	};
	uint8_t prov[512];
	size_t plen = build_prov_v2(prov, sizeof(prov), 2, TS_DEPLOY_STANDALONE, 1);
	ts_ch_state_t st;

	zassert_equal(ts_store_prov_write_test(prov, plen), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	ts_safety_test_reset();
	zassert_equal(ts_safety_register_channel(&led_ch), TS_OK);
	ts_net_test_reset();

	/* standalone boot 链路声明（net/init.c 同型镜像）：输出 ACTIVE、值保持
	 * poweron 安全值（DR-04 无回写） */
	ts_safety_set_link(true);
	zassert_equal(ts_safety_channel_state("saled", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE);

	/* hb 判定建立（滞回 2 次）——standalone 不额外迁移（幂等 ACTIVE） */
	ts_net_linkmon_hb_host(100);
	ts_net_linkmon_hb_host(1100);
	zassert_true(ts_net_link_up());
	zassert_equal(ts_safety_commit("saled", (ts_out_value_t){.b = true}), TS_OK,
		      "本地逻辑照常（写路径开放）");

	/* 断链超时：判定 down + 事件留痕，但状态零迁移（§3a）——仍 ACTIVE、
	 * 写路径不受断链影响 */
	ts_net_linkmon_tick(1100 + 6 * 1000 + 1);
	zassert_false(ts_net_link_up(), "判定状态照常翻转（观测面不变）");
	zassert_equal(ts_safety_channel_state("saled", &st), TS_OK);
	zassert_equal(st, TS_ST_ACTIVE, "standalone 断链零迁移");
	zassert_equal(ts_safety_commit("saled", (ts_out_value_t){.b = false}), TS_OK,
		      "断链后本地写照常（合同 3 模式化）");

	/* 对照：orchestrated（v1 缺省）同型驱动 → LINKLOSS 迁移（现行为不变） */
	plen = build_prov_v2(prov, sizeof(prov), 1, 0, 0);
	zassert_equal(ts_store_prov_write_test(prov, plen), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	ts_net_test_reset(); /* linkmon 判定复位（通道表保留） */
	ts_net_linkmon_hb_host(100);
	ts_net_linkmon_hb_host(1100);
	zassert_true(ts_net_link_up());
	ts_net_linkmon_tick(1100 + 6 * 1000 + 1);
	zassert_equal(ts_safety_channel_state("saled", &st), TS_OK);
	zassert_equal(st, TS_ST_SAFE_LINKLOSS, "orchestrated 断链迁移（对照）");
	zassert_equal(ts_safety_commit("saled", (ts_out_value_t){.b = true}), TS_E_STATE,
		      "orchestrated 断链写拒（对照）");
}

/* LLD-ts-safety §5a：estop_remote_clear=0 → 远程清除拒（有本地交互设备
 * 保守缺省；v1 缺字段 = enabled 现网兼容） */
ZTEST(framework_net, test_15_estop_clear_remote_disabled)
{
	uint8_t req[192];
	uint8_t resp[192];
	size_t rlen;
	size_t rl2 = 0;
	uint8_t prov[512];
	size_t plen = build_prov_v2(prov, sizeof(prov), 2, TS_DEPLOY_ORCHESTRATED, 0);

	zassert_equal(ts_store_prov_write_test(prov, plen), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	ts_net_test_reset();
	ts_net_cmd_test_reset();
	zassert_equal(ts_net_cmd_sys_init(), TS_OK);

	/* 正确令牌仍被拒（通道结构性禁用——TS_E_STATE + need local-clear） */
	rlen = build_req(req, sizeof(req), "estop-clear", "confirm", "estop", 0);
	zassert_equal(ts_net_cmd_dispatch("sys/estop-clear", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_STATE);
	zassert_not_null(strstr((char *)resp, "local-clear"), "回执带 need=local-clear");

	/* 恢复 enabled（v1 缺省等义）→ 既有放行语义 */
	plen = build_prov_v2(prov, sizeof(prov), 1, 0, 0);
	zassert_equal(ts_store_prov_write_test(prov, plen), TS_OK);
	zassert_equal(ts_store_prov_load(), TS_OK);
	rlen = build_req(req, sizeof(req), "estop-clear", "confirm", "estop", 0);
	zassert_equal(ts_net_cmd_dispatch("sys/estop-clear", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);
}

/* CG3（Q-41②/合同 11 · LLD-ts-safety §5b）：本地席位 = 租约强制失效视图 -- */
ZTEST(framework_net, test_16_arb_lease_view)
{
	uint8_t req[192];
	uint8_t resp[192];
	size_t rlen;
	size_t rl2 = 0;

	ts_net_test_reset();
	ts_net_cmd_test_reset();
	ts_net_lease_test_reset();
	ts_safety_test_reset();
	zassert_equal(ts_net_cmd_sys_init(), TS_OK);
	zassert_equal(ts_safety_set_arb(TS_ARB_REMOTE), TS_OK, "缺省席主（显式定基）");

	/* 基线：远程 acquire OK；gated 命令过租约门（app-begin 到 handler 层） */
	rlen = build_req_v2(req, sizeof(req), "lease-acquire", "rid-arb1",
			    "holder", "t-agent", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK);
	rlen = build_req_v2(req, sizeof(req), "app-begin", "rid-arb2", NULL, NULL,
			    0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-begin", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_PARAM,
		      "过租约门（拒因来自 handler 参数面，非租约）");

	/* 本地激活：acquire 拒 + local_active 装饰；gated 命令 = 租约失效视图拒 */
	zassert_equal(ts_safety_set_arb(TS_ARB_LOCAL), TS_OK);
	rlen = build_req_v2(req, sizeof(req), "lease-acquire", "rid-arb3",
			    "holder", "t-agent", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_STATE);
	zassert_not_null(strstr((char *)resp, "local_active"), "降级回执带归因");
	rlen = build_req_v2(req, sizeof(req), "app-begin", "rid-arb4", NULL, NULL,
			    0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/app-begin", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_STATE,
		      "本地激活期 gated 写命令 = 租约失效视图拒");

	/* 释放：远程可重新 acquire */
	zassert_equal(ts_safety_set_arb(TS_ARB_REMOTE), TS_OK);
	rlen = build_req_v2(req, sizeof(req), "lease-acquire", "rid-arb5",
			    "holder", "t-agent", 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/lease-acquire", req, rlen, resp,
					  sizeof(resp), &rl2), TS_OK,
		      "本地释放后远程重新授予");

	/* CG5-B 构建面裁剪：本套件无 TS_HAL_FS → fs-* 命令不在面（NOTFOUND 如实） */
	rlen = build_req_v2(req, sizeof(req), "fs-begin", "rid-arb6", NULL, NULL,
			    0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/fs-begin", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_NOTFOUND,
		      "无 fs 能力构建 = 命令面不存在");

	/* EC2 构建面裁剪：本套件无 TS_STORE_TRUST → trust-* 命令不在面
	 * （Agent 探测面：旧固件/未使能设备 = NOTFOUND 如实） */
	rlen = build_req_v2(req, sizeof(req), "trust-get", "rid-arb7", NULL, NULL,
			    0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/trust-get", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_NOTFOUND,
		      "无信任表构建 = 命令面不存在");
	rlen = build_req_v2(req, sizeof(req), "trust-update", "rid-arb8", NULL,
			    NULL, 0, NULL, false, 0);
	zassert_equal(ts_net_cmd_dispatch("sys/trust-update", req, rlen, resp,
					  sizeof(resp), &rl2), TS_E_NOTFOUND,
		      "同上（gated 命令亦不在面）");
}

/* ---- L2：open 阻塞体喂狗豁免接线（R1·DEC-64①，LLD-ts-net §2）------------- */

ZTEST(framework_net, test_17_open_grace_wiring)
{
	/* 本套件 wdt 首次注册（巡检不在本套件运行——仅经 seam 查询）；
	 * 契约：session_poll 于 open 前置 grace、返回后 feed 清窗，成败同路 */
	zassert_equal(ts_wdt_register(TS_WDT_NET, 100), TS_OK);
	ts_wdt_feed(TS_WDT_NET); /* 激活（grace 仅对激活源生效） */

	ts_net_test_reset();
	open_grace_seen = false;
	fake_open_fail = 1;
	fake_up_v = false;
	ts_net_set_transport(&fake_t);
	zassert_equal(ts_net_session_poll(0), TS_NET_DOWN, "open 失败（入退避）");
	zassert_true(open_grace_seen, "grace 于 open 调用体内置位");
	zassert_false(ts_wdt_grace_active(TS_WDT_NET), "open 返回后 feed 清窗");

	/* 成功路同语义（退避 250ms 后建链） */
	open_grace_seen = false;
	fake_open_fail = 0;
	fake_up_v = true;
	zassert_equal(ts_net_session_poll(250), TS_NET_CONNECTED, "open 成功");
	zassert_true(open_grace_seen, "成功路 open 体内 grace 同样置位");
	zassert_false(ts_wdt_grace_active(TS_WDT_NET), "成功路 feed 同样清窗");
}

ZTEST_SUITE(framework_net, NULL, NULL, NULL, NULL, NULL);
