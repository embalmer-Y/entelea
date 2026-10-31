/* SPDX-License-Identifier: Apache-2.0 */
/* zenoh-pico 传输实现（CONFIG_TS_NET_ZENOH；钉版 1.10.1，DR-22 三方同 minor）。
 * 副作用边界：z_* 调用全部收敛于本文件（传输缝纪律，LLD-ts-net §1）。
 * locator 来自 prov.router_locators[0]（DEC-20 client 角色）；缺省回退
 * tcp/127.0.0.1:7447（native_sim 基线，DEC-27；DEC-40 起缺省亦走 TCP）。
 * M3a.2：建链时接线 queryable（cube 前缀尾通配声明 + 回调内过滤 cmd 尾段
 * → ts_net_cmd_dispatch）与 hb-host 订阅（→ ts_net_linkmon_hb_host）。
 * DEC-40 批：locator 形态校验（命令面链路必须 TCP/TLS——UDP 拒绝）；is_up =
 * zp 任务活性自省（会话在而任务死 = 半死态，LLD §2）；publish 承接 QoS
 * 映射（DEC-42：安全事件 = BLOCK + REAL_TIME，缺省 = DROP + DATA）。 */
#include <stdio.h>
#include <string.h>
#include <zenoh-pico.h>
#include <ts/core.h>
#include <ts/net.h>
#include <ts/store.h>
#include "internal.h"

#define RESP_MAX 512 /* sys 命令回执预算（get-audit 最坏编码内） */

static z_owned_session_t zs;
static bool opened;
static z_owned_queryable_t zq;
static z_owned_subscriber_t zsub;

/* ---- 回调（zenoh 内部线程上下文；直落框架确定性分发）--------------------- */

static void drop_query(void *ctx)
{
	ARG_UNUSED(ctx);
}

static void on_query(z_loaned_query_t *query, void *ctx)
{
	ARG_UNUSED(ctx);
	/* 两种命令面：…/<uid>/cmd（实例命令，M2b.2+）与 …/sys/<cmd>（sys 面，
	 * LLD-ts-net §3/§4）——按 key 形态剥前缀取分发后缀 */
	z_view_string_t vs;
	const z_loaned_keyexpr_t *qk = z_query_keyexpr(query);

	if (z_keyexpr_as_view_string(qk, &vs) != Z_OK) {
		return;
	}
	const char *k = z_string_data(z_loan(vs));
	size_t klen = z_string_len(z_loan(vs));

	/* 通配 query（发现类，MA3.1）：zenoh-pico 无 keyexpr 改写，回调收到原始
	 * 通配 key（形如 "tessera" + 两段单星 + "sys" 段形态）——按本 cube 前缀
	 * 具体化：node/cube 两段为单星时以自身代入，字面段必须与本 cube 一致；
	 * 深层通配（第三段起含星号）与形态外 key 不回执（客户端超时可见）。 */
	static char ckey[96];

	if (memchr(k, '*', klen) != NULL) {
		size_t plen = strlen(ts_net_prefix);

		if (plen < 10 || klen < 10 ||
		    strncmp(k, "tessera/", 8) != 0 ||
		    strncmp(ts_net_prefix, "tessera/", 8) != 0) {
			return;
		}
		/* 两把 key 的 chunk 边界：q = 查询，p = 本 cube 前缀 */
		size_t qc1 = 8, qc1e = qc1, qc2, qc2e;

		while (qc1e < klen && k[qc1e] != '/') {
			qc1e++;
		}
		qc2 = qc1e + 1;
		qc2e = qc2;
		while (qc2e < klen && k[qc2e] != '/') {
			qc2e++;
		}
		if (qc1e >= klen || qc2e >= klen) {
			return; /* 段数不足（< node/cube/rest 三段） */
		}
		size_t rest = qc2e + 1;

		if (memchr(k + rest, '*', klen - rest) != NULL) {
			return; /* 深层通配：不猜语义 */
		}
		size_t pc1 = 8, pc1e = pc1;

		while (pc1e < plen && ts_net_prefix[pc1e] != '/') {
			pc1e++;
		}
		/* chunk1（node）：'*' → 自身；否则须字面一致 */
		bool c1_star = (qc1e - qc1 == 1 && k[qc1] == '*');
		size_t c1_len = c1_star ? pc1e - pc1 : qc1e - qc1;

		if (!c1_star &&
		    (pc1e - pc1 != c1_len ||
		     strncmp(k + qc1, ts_net_prefix + pc1, c1_len) != 0)) {
			return;
		}
		/* chunk2（cube）：同上 */
		bool c2_star = (qc2e - qc2 == 1 && k[qc2] == '*');
		size_t c2_len = c2_star ? plen - (pc1e + 1) : qc2e - qc2;

		if (!c2_star &&
		    (plen - (pc1e + 1) != c2_len ||
		     strncmp(k + qc2, ts_net_prefix + pc1e + 1, c2_len) != 0)) {
			return;
		}
		if ((size_t)snprintf(ckey, sizeof(ckey), "tessera/%.*s/%.*s/%.*s",
				     (int)c1_len, c1_star ? ts_net_prefix + pc1 : k + qc1,
				     (int)c2_len, c2_star ? ts_net_prefix + pc1e + 1 : k + qc2,
				     (int)(klen - rest), k + rest) >= sizeof(ckey)) {
			return;
		}
		k = ckey;
		klen = strlen(ckey);
	}
	size_t plen = strlen(ts_net_prefix);

	if (klen <= plen + 1 || strncmp(k, ts_net_prefix, plen) != 0) {
		return; /* 非 cube 命名空间：不回执（客户端侧超时可见） */
	}
	static char suffix[64];
	size_t slen;
	bool is_sys = (klen > plen + 5 && strncmp(k + plen, "/sys/", 5) == 0);

	if (is_sys) {
		slen = klen - (plen + 1); /* "sys/<cmd>" */
	} else if (klen > plen + 4 && strcmp(k + klen - 4, "/cmd") == 0) {
		slen = klen - 4 - (plen + 1); /* "<uid>" */
	} else {
		return;
	}
	if (slen == 0 || slen >= sizeof(suffix)) {
		return;
	}
	memcpy(suffix, k + plen + 1, slen);
	suffix[slen] = '\0';

	const z_loaned_bytes_t *pl = z_query_payload(query);
	z_bytes_reader_t rd = z_bytes_get_reader(pl);
	/* 部署面请求上限 = 块上限 + 信封余量（MA3.1，LLD-agent-deploy-net §3）；分发回调
	 * 串行（单 query 任务线程），静态缓冲与既有纪律一致 */
	static uint8_t req[CONFIG_TS_NET_APP_CHUNK_MAX + 512];
	size_t rlen = z_bytes_reader_read(&rd, req, sizeof(req));

	static uint8_t resp[RESP_MAX];
	size_t resp_len = 0;

	(void)ts_net_cmd_dispatch(suffix, req, (uint32_t)rlen, resp, sizeof(resp),
				  &resp_len);
	/* 常设观测面（F5 已定案后留任）：sys 命令为部署级低频面，
	 * 每命令一行后缀/长度记录 = 排障第一落点（K3 空回执排查实证价值）。 */
	printk("[q] %s rlen=%u resp_len=%u\n", suffix, (unsigned)rlen,
	       (unsigned)resp_len);
	/* 回执（reply key = 查询 key 原样回显）。
	 * F5 修正（K3 夜间）：z_bytes_copy_from_buf 拷贝构造——zenoh-pico 的
	 * 网络消息组件为别名（_z_send_reply 源码实证），非拥有视图在
	 * Z_CONGESTION_CONTROL_BLOCK 排队期间可被下一查询的静态 resp 覆写
	 * （真机确定性空回执/损坏回执实证）；拥有拷贝消除竞态窗口。 */
	z_owned_bytes_t out;

	if (z_bytes_copy_from_buf(&out, resp, resp_len) == Z_OK) {
		z_query_reply_options_t ro;

		z_query_reply_options_default(&ro);
		(void)z_query_reply(query, qk, z_move(out), &ro);
	}
}

static void on_sample(z_loaned_sample_t *sample, void *ctx)
{
	ARG_UNUSED(ctx);
	/* 仅 hb-host 订阅在册；到达即视为 host 心跳（DR-12，合同 8 本地判定） */
	ts_net_linkmon_hb_host(ts_time_ms());
}

/* ---- 传输实现 ------------------------------------------------------------ */

/* 命令面链路约束（DEC-40）：TCP/TLS 前缀之外的 locator（如 udp/）拒绝建链
 * ——UDP 仅限 scouting/遥测可选路径；zenoh query 无重传，命令-回执链路的
 * 传输层可靠性由 TCP/TLS 承载。 */
static bool locator_cmd_face_ok(const char *loc)
{
	return strncmp(loc, "tcp/", 4) == 0 || strncmp(loc, "tls/", 4) == 0;
}

#ifdef CONFIG_TS_NET_TLS
/* TLS mTLS 供给（P1-2 · DEC-63③）：prov v2 三元组（DER）→ base64 配置键
 * 注入（zenoh-pico tls_stream 消费 *_BASE64 键——DER 解析路径）。三键同现
 * = 客户端双向认证（服务器验 CA + 服务器验设备证书）；名称校验维持缺省
 * VERIFY_REQUIRED（服务器证书 SAN 须匹配 locator 主机——证书工具随 SAN
 * 签发，不做信任降级）。tls/ locator 而无三元组 = fail-closed 拒建链。 */
#include <mbedtls/base64.h>

/* base64 编码暂存（开链期专用，静态避 net 线程栈峰）——DEC-63③ 常量上限派生 */
static char b64_ca[((TS_PROV_TLS_CA_MAX + 2) / 3) * 4 + 1];
static char b64_cert[((TS_PROV_TLS_CERT_MAX + 2) / 3) * 4 + 1];
static char b64_key[((TS_PROV_TLS_KEY_MAX + 2) / 3) * 4 + 1];

/* zenoh-pico 补丁钩子（DEC-63①）：阻塞握手脚本内喂 NET 看门狗——
 * TLS 握手（PSA 运算 + 往返）可超过 NET 喂狗周期（2s），无心跳 = 饿死复位
 * （真机实证：握手期 RTC_SW_CPU_RST + 无 noinit 留痕 = task_wdt 直通路径）。 */
void z_tls_handshake_progress_hook(void)
{
	ts_wdt_feed(TS_WDT_NET);
}

static bool b64_of(const uint8_t *der, uint16_t der_len, char *out, size_t cap)
{
	size_t olen = 0;

	if (mbedtls_base64_encode((unsigned char *)out, cap, &olen,
				  der, der_len) != 0) {
		return false;
	}
	out[olen] = '\0';
	return true;
}

static ts_res_t tls_config_insert(z_loaned_config_t *cfg,
				  const ts_prov_t *prov, const char *loc)
{
	if (strncmp(loc, "tls/", 4) != 0) {
		if (prov->tls_ca_len != 0) {
			printk("[ts-net] tls: prov 带三元组而 locator 非 tls/（明文运行——配置不一致如实登记）\n");
		}
		return TS_OK;
	}
	if (prov->tls_ca_len == 0 || prov->tls_cert_len == 0 ||
	    prov->tls_key_len == 0) {
		printk("[ts-net] tls: locator tls/ 而 prov 无三元组（fail-closed 拒建链，DEC-63③）\n");
		return TS_E_PARAM;
	}

	const ts_prov_tls_t *t = ts_store_prov_tls();

	if (!b64_of(t->ca, t->ca_len, b64_ca, sizeof(b64_ca)) ||
	    !b64_of(t->cert, t->cert_len, b64_cert, sizeof(b64_cert)) ||
	    !b64_of(t->key, t->key_len, b64_key, sizeof(b64_key)) ||
	    zp_config_insert(cfg, Z_CONFIG_TLS_ROOT_CA_CERTIFICATE_BASE64_KEY,
			     b64_ca) != Z_OK ||
	    zp_config_insert(cfg, Z_CONFIG_TLS_CONNECT_CERTIFICATE_BASE64_KEY,
			     b64_cert) != Z_OK ||
	    zp_config_insert(cfg, Z_CONFIG_TLS_CONNECT_PRIVATE_KEY_BASE64_KEY,
			     b64_key) != Z_OK ||
	    zp_config_insert(cfg, Z_CONFIG_TLS_ENABLE_MTLS_KEY, "true") != Z_OK) {
		printk("[ts-net] tls: mTLS 配置注入 FAIL\n");
		return TS_E_IO;
	}
	printk("[ts-net] tls: mTLS 注入 ca=%uB cert=%uB key=%uB（名称校验 ON）\n",
	       (unsigned)prov->tls_ca_len, (unsigned)prov->tls_cert_len,
	       (unsigned)prov->tls_key_len);
	return TS_OK;
}
#endif /* CONFIG_TS_NET_TLS */

static ts_res_t zenoh_open(void)
{
	if (opened) {
		return TS_OK;
	}
	const ts_prov_t *prov = ts_store_prov();
	/* prov 未加载/未烧录时取默认（缺省亦 TCP——DEC-40 命令面链路约束） */
	const char *loc = (prov->router_locators[0][0] != '\0')
				  ? prov->router_locators[0]
				  : "tcp/127.0.0.1:7447";

	if (!locator_cmd_face_ok(loc)) {
		printk("[ts-net] zenoh_open: locator %s rejected (DEC-40: tcp//tls/ only)\n",
		       loc);
		return TS_E_PARAM;
	}

	z_owned_config_t cfg;

	if (z_config_default(&cfg) != Z_OK) {
		printk("[ts-net] zenoh_open: config_default FAIL\n");
		return TS_E_IO;
	}
	if (zp_config_insert(z_loan_mut(cfg), Z_CONFIG_CONNECT_KEY, loc) != Z_OK) {
		printk("[ts-net] zenoh_open: insert loc=%s FAIL\n", loc);
		z_drop(z_move(cfg));
		return TS_E_IO;
	}
#ifdef CONFIG_TS_NET_TLS
	ts_res_t tr = tls_config_insert(z_loan_mut(cfg), prov, loc);

	if (tr != TS_OK) {
		z_drop(z_move(cfg));
		return tr;
	}
#endif
	z_result_t or = z_open(&zs, z_move(cfg), NULL);

	if (or != Z_OK) {
		printk("[ts-net] zenoh_open: z_open rc=%d loc=%s\n", (int)or, loc);
		z_drop(z_move(zs)); /* 归零 owned 句柄，允许重试 open */
		return TS_E_IO;
	}
	opened = true;
	printk("[ts-net] zenoh_open: CONNECTED loc=%s\n", loc);

	/* queryable：cube 前缀下 ** 通配（** 只能作尾段——回调内过滤 "/cmd"） */
	z_owned_keyexpr_t qk;
	char qkey[80];
	int qw = snprintf(qkey, sizeof(qkey), "%s/**", ts_net_prefix);

	if (qw < 0 || (size_t)qw >= sizeof(qkey) ||
	    z_keyexpr_from_str(&qk, qkey) != Z_OK) {
		goto fail;
	}
	z_owned_closure_query_t qclos;

	z_closure_query(&qclos, on_query, drop_query, NULL);
	if (z_declare_queryable(z_loan(zs), &zq, z_loan(qk), z_move(qclos), NULL) != Z_OK) {
		z_drop(z_move(qk));
		goto fail;
	}
	z_drop(z_move(qk));

	/* 订阅 host 心跳 …/sys/hb-host（DR-12） */
	z_owned_keyexpr_t hk;
	char hkey[80];
	int hw = snprintf(hkey, sizeof(hkey), "%s/sys/hb-host", ts_net_prefix);

	if (hw < 0 || (size_t)hw >= sizeof(hkey) ||
	    z_keyexpr_from_str(&hk, hkey) != Z_OK) {
		goto fail;
	}
	z_owned_closure_sample_t sclos;

	z_closure_sample(&sclos, on_sample, NULL, NULL);
	if (z_declare_subscriber(z_loan(zs), &zsub, z_loan(hk), z_move(sclos), NULL) != Z_OK) {
		z_drop(z_move(hk));
		goto fail;
	}
	z_drop(z_move(hk));
	return TS_OK;
fail:
	z_drop(z_move(zs));
	opened = false;
	return TS_E_IO;
}

static void zenoh_close(void)
{
	if (opened) {
		z_undeclare_subscriber(z_move(zsub));
		z_undeclare_queryable(z_move(zq));
		z_drop(z_move(zs));
		opened = false;
	}
}

static ts_res_t zenoh_publish(const char *key, const uint8_t *payload, uint32_t len,
			      ts_net_qos_t qos)
{
	if (!opened) {
		return TS_E_STATE;
	}
	z_owned_keyexpr_t ke;

	if (z_keyexpr_from_str(&ke, key) != Z_OK) {
		return TS_E_PARAM;
	}
	z_owned_bytes_t pl;

	/* V2-1 根治（数据竞争）：z_put 异步发送，bytes 生命周期须覆盖发送
	 * 完成点——static_buf（别名/零拷贝）在调用方为栈缓冲或环形复用槽
	 * （pubq/avq 出队形态）时构成 use-after-overwrite：入队时 crc 正确、
	 * 发送时字节已被后续条目覆盖——形态谱系 = 1KB chunk crc 100% 坏
	 * （部分覆盖）→ 高频连发帧级损坏被 router 整包丢弃（64/256B 档
	 * 0 到达）→ 低频心跳无竞争到达。修复 = 拷贝语义（一次 memcpy，
	 * payload ≤1KB 可接受）。 */
	if (z_bytes_copy_from_buf(&pl, payload, (size_t)len) != Z_OK) {
		z_drop(z_move(ke));
		return TS_E_IO;
	}
	z_put_options_t opt;

	z_put_options_default(&opt);
	if (qos == TS_NET_QOS_SAFETY) {
		/* DEC-42 QoS 映射：安全事件阻塞式（不丢）+ 高优先级 lane */
		opt.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
		opt.priority = Z_PRIORITY_REAL_TIME;
	}
	z_result_t r = z_put(z_loan(zs), z_loan(ke), z_move(pl), &opt);

	z_drop(z_move(ke));
	return (r == Z_OK) ? TS_OK : TS_E_IO;
}

static bool zenoh_is_up(void)
{
	if (!opened) {
		return false;
	}
#if defined(Z_FEATURE_MULTI_THREAD) && Z_FEATURE_MULTI_THREAD == 1
	/* 传输健康三判据（LLD §2 R5-V 修订）。①② 执行器任务活性——1.10.1
	 * 执行器模型下两个 zp_*_is_running 均退化为「后台执行器线程存活」：
	 * 租期/读 fut 判死退出（AUTO_RECONNECT=0 不再生重开 fut）后线程照常
	 * 轮转，单看任务活性 is_up 恒真 = 永久僵尸（2026-10-18 真机实证：部署
	 * 突发击链后 30min+ 无重连，板 ping 通而 zenoh 全哑）。③ 传输态本体
	 * ——全部死法（租期判死/读任务失败/半开由租期兜底）在一个租期内汇入
	 * _state=CLOSED，pico 补丁加 zp_transport_is_open 访问器作真信号。 */
	return zp_read_task_is_running(z_loan(zs)) &&
	       zp_lease_task_is_running(z_loan(zs)) &&
	       zp_transport_is_open(z_loan(zs));
#else
	return true; /* 单线程构建退化：opened 即活（无后台任务可自省） */
#endif
}

const ts_net_transport_t ts_net_zenoh_transport = {
	.open = zenoh_open,
	.close = zenoh_close,
	.publish = zenoh_publish,
	.is_up = zenoh_is_up,
};
