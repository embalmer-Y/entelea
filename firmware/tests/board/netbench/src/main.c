/* SPDX-License-Identifier: Apache-2.0 */
/* netbench（板级四单元）：WiFi + zenoh 命令往返时延——xiao_esp32s3 真机。
 * 流程：WiFi STA 连接（凭证经 cmake 变量注入，不入仓库）→ DHCP →
 * prov 烧入（TS_TEST 通道；定稿键序 CBOR，locator = NETBENCH_ROUTER）→
 * ts_core_boot()（含 net_init → zenoh-pico TCP 会话 + 心跳/命令面）。
 * 时延测量在 PC 侧客户端（netbench_client.py，L0/L1/L2 三层）。
 * 板级七追加（NB-R 重连场景）：Zephyr esp32 WiFi 驱动无自动重连——断线
 * 事件 → 固定 2s 周期重试 CONNECT（无抖动，与 ts-net 会话退避同纪律
 * 〔DEC-27〕）；zenoh 会话恢复由 ts-net 自带状态机承担（session_poll 检测
 * is_up → close → 退避重 open）。脚本化双断链（设备侧强制断开 = 与 AP 侧
 * 掉线在 STA 视角等价：断线事件 + 重试路径同一代码）真机验证。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h> /* 须在 net_if.h 之后（参数表内 struct net_if 可见性） */
#include <zephyr/sys/atomic.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <ts/net.h>
#include <ts/safety.h>
#include <ts/store.h>

#ifndef NETBENCH_WIFI_SSID
#define NETBENCH_WIFI_SSID ""
#endif
#ifndef NETBENCH_WIFI_PASSWORD
#define NETBENCH_WIFI_PASSWORD ""
#endif
#ifndef NETBENCH_ROUTER
#define NETBENCH_ROUTER "tcp/192.168.2.90:7447"
#endif

static struct net_mgmt_event_callback ip_cb;
static struct net_mgmt_event_callback wifi_cb;
static struct k_sem ip_ready;
static atomic_t wifi_assoc;     /* 关联+DHCP 完成（重连成功判据） */
static atomic_t assoc_ok_evt;   /* 事件标志：connect_result(status=0) 待处理 */
static atomic_t disconn_evt;    /* 事件标志：disconnect 待处理 */
static struct k_work_delayable wifi_retry_work;
static struct k_work assoc_work;
static struct wifi_connect_req_params cnx; /* 凭证参数（init 一次，重连复用） */

static void wifi_try_connect(struct k_work *w)
{
	ARG_UNUSED(w);
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, net_if_get_default(),
			  &cnx, sizeof(cnx));

	printk("NBR1 retry connect rc=%d t=%u\n", rc,
	       (unsigned)k_uptime_get_32());
	if (rc != 0) {
		/* 请求本身被拒（驱动忙/状态机未复位）→ 下一周期再试 */
		k_work_reschedule(&wifi_retry_work, K_SECONDS(2));
	}
}

static void wifi_assoc_work(struct k_work *w)
{
	/* net_mgmt 一律在工作项上下文执行——事件回调内调 net_mgmt 会重入
	 * 自激（板级七真机实证：PS/DHCP 请求在回调内 = connect/ADDR 事件
	 * 每 ~40ms 风暴）。 */
	ARG_UNUSED(w);
	struct net_if *iface = net_if_get_default();

	if (atomic_set(&disconn_evt, 0) == 1) {
		atomic_set(&wifi_assoc, 0);
		printk("NBw disconnect t=%u\n", (unsigned)k_uptime_get_32());
		/* 同一恢复路径（不区分 AP 掉线/本端强制/驱动复位）：
		 * ① 媒体掉线下沉 ts-net（TCP 半开立即判 DOWN）；
		 * ② DHCP 重启（esp32 口断线不清租约）；
		 * ③ 固定 2s 重试关联。 */
		ts_net_session_media_down();
		net_dhcpv4_restart(iface);
		k_work_reschedule(&wifi_retry_work, K_SECONDS(2));
	}
	if (atomic_set(&assoc_ok_evt, 0) == 1) {
		/* 关联成功 = 判据达成（esp32 口断线不清 IPv4，重连同址无
		 * ADDR_ADD 事件——板级七实证）；省电幂等重设。 */
		atomic_set(&wifi_assoc, 1);
		struct wifi_ps_params ps = {.enabled = WIFI_PS_DISABLED};

		(void)net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
		printk("NBw associated t=%u\n", (unsigned)k_uptime_get_32());
	}
}

static void ip_handler(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (event == NET_EVENT_IPV4_ADDR_ADD && net_if_is_up(iface)) {
		k_sem_give(&ip_ready);
		printk("NBR2 ipv4 up t=%u\n", (unsigned)k_uptime_get_32());
	}
}

static void wifi_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	/* 只置标志 + 提交工作项；严禁在此上下文调 net_mgmt（重入自激） */
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;

		if (st != NULL && st->status == 0) {
			atomic_set(&assoc_ok_evt, 1);
		}
		printk("NBw connect_result status=%d t=%u\n",
		       st ? (int)st->status : -1, (unsigned)k_uptime_get_32());
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;

		printk("NBw disconnect_result reason=%d t=%u\n",
		       st ? (int)st->status : -1, (unsigned)k_uptime_get_32());
		atomic_set(&disconn_evt, 1);
	}
	if (atomic_get(&assoc_ok_evt) == 1 || atomic_get(&disconn_evt) == 1) {
		k_work_submit(&assoc_work);
	}
}

/* {"v":1,"node_id":"nbn","cube_id":"cbn","routers":[NETBENCH_ROUTER 字面]…
 * 定稿键序（prov.c 固定走查序；重生成脚本 ~/project/logs/gen_prov.py）。
 * 注：router 字面量为生成期固化的 tcp/192.168.2.90:7447——PC 侧 IP 变更
 * 时须重生成（含此注释如实登记）。 */
static const uint8_t prov_blob[] = {
	0xa9, 0x61, 0x76, 0x01, 0x67, 0x6e, 0x6f, 0x64, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x6e, 0x62, 0x6e, 0x67, 0x63, 0x75, 0x62, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x63, 0x62, 0x6e, 0x67, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x72, 0x73,
	0x81, 0x75, 0x74, 0x63, 0x70, 0x2f, 0x31, 0x39, 0x32, 0x2e, 0x31, 0x36,
	0x38, 0x2e, 0x32, 0x2e, 0x39, 0x30, 0x3a, 0x39, 0x39, 0x35, 0x35, 0x63,
	0x70, 0x6b, 0x30, 0x58, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x63, 0x70, 0x6b, 0x31, 0x58, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x64, 0x63, 0x72, 0x65, 0x64, 0x60, 0x66, 0x70, 0x77,
	0x72, 0x5f, 0x6d, 0x61, 0x19, 0x01, 0xf4, 0x65, 0x65, 0x73, 0x74, 0x6f,
	0x70, 0x00,
};

static const ts_out_ch_t nb_led = {
	.uid = "nbled", .kind = TS_CH_GPIO,
	.poweron = {.b = false}, .linkloss = {.b = false}, .fault = {.b = false},
};
static const ts_hal_dev_desc_t nb_dev = {
	.uid = "nbled", .kind = TS_DEV_GPIO_OUT,
};

/* ---- NB-R 脚本化断链场景（板级七）：稳态后设备侧强制断开 ×2，全链自愈
 * 观测（WiFi 重连 → DHCP → zenoh 会话恢复〔ts-net 自带退避〕→ 命令面
 * 恢复）。断链窗口内 PC 侧客户端 L1 连续探测 = 恢复时间线对侧证据。 */
static void nbr_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* 等 zenoh 会话建立（ts-net 周期驱动经 net_init 起来） */
	for (int i = 0; i < 300 && ts_net_state() != TS_NET_CONNECTED; i++) {
		k_msleep(100);
	}
	if (ts_net_state() != TS_NET_CONNECTED) {
		printk("NBR FAIL no session\n");
		return;
	}
	printk("NBR ready t=%u（15s 稳态窗口后第 1 次强制断链）\n",
	       (unsigned)k_uptime_get_32());
	k_msleep(15000);

	for (int cycle = 1; cycle <= 2; cycle++) {
		uint32_t t_drop = k_uptime_get_32();

		printk("NBR0 forced drop #%d t=%u\n", cycle, t_drop);
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, net_if_get_default(),
			       NULL, 0);
		/* 恢复判据：WiFi 关联 + zenoh 会话再连（时间戳差 = 全链自愈时长） */
		bool recovered = false;

		for (int i = 0; i < 600; i++) {
			if (atomic_get(&wifi_assoc) == 1 &&
			    ts_net_state() == TS_NET_CONNECTED) {
				recovered = true;
				break;
			}
			k_msleep(200);
		}
		uint32_t t_rec = k_uptime_get_32();

		printk("NBR9 %s #%d drop=%u recover=%u 全链自愈=%ums\n",
		       recovered ? "recovered" : "TIMEOUT", cycle, t_drop, t_rec,
		       t_rec - t_drop);
		if (!recovered) {
			printk("NBR FAIL recover\n");
			return;
		}
		if (cycle == 1) {
			k_msleep(10000); /* 第 2 轮前稳态窗口 */
		}
	}
	printk("NBR PASS cycles=2\n");
	for (;;) {
		k_msleep(10000);
		printk("NBR alive t=%u assoc=%d net=%d\n",
		       (unsigned)k_uptime_get_32(),
		       (int)atomic_get(&wifi_assoc), (int)ts_net_state());
	}
}
K_THREAD_DEFINE(nbr_tid, 4096, nbr_thread, NULL, NULL, NULL, 13, 0, 0);

int main(void)
{
	printk("NB0 netbench start（zenoh locator 来自 prov blob，见下方 NB2 后日志）\n");

	struct net_if *iface = net_if_get_default();

	k_sem_init(&ip_ready, 0, 1);
	k_work_init_delayable(&wifi_retry_work, wifi_try_connect);
	k_work_init(&assoc_work, wifi_assoc_work);
	net_mgmt_init_event_callback(&ip_cb, ip_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ip_cb);
	net_mgmt_init_event_callback(&wifi_cb, wifi_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	static const uint8_t ssid[] = NETBENCH_WIFI_SSID;
	static const uint8_t psk[] = NETBENCH_WIFI_PASSWORD;

	cnx = (struct wifi_connect_req_params){
		.ssid = ssid,
		.ssid_length = sizeof(ssid) - 1,
		.psk = psk,
		.psk_length = sizeof(psk) - 1,
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
	};

	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &cnx, sizeof(cnx));

	printk("NB0 wifi_connect rc=%d ssid=%s\n", rc, NETBENCH_WIFI_SSID);
	/* 关 WiFi 省电：默认 modem-sleep 对齐信标 → 命令往返尾部 ~DTIM(≈100ms)
	 * 量级（板级四实证：开 PS 时 L1 p50≈66ms/max≈105ms）。 */
	struct wifi_ps_params ps = {
		.enabled = WIFI_PS_DISABLED,
	};

	int ps_rc = net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));

	printk("NB0 ps_disable rc=%d\n", ps_rc);
	if (k_sem_take(&ip_ready, K_SECONDS(40)) != 0) {
		printk("NB FAIL dhcp timeout\n");
		return 1;
	}
	char buf[NET_IPV4_ADDR_LEN];

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (a->is_used) {
			net_addr_ntop(AF_INET, &a->address.in_addr, buf, sizeof(buf));
			printk("NB1 wifi up ip=%s\n", buf);
		}
	}

	/* prov：RAM 后端每次启动烧入（真板持久化 = 板级后续任务） */
	ts_res_t lr = ts_store_prov_load();
	ts_res_t wr = (lr == TS_OK)
		? TS_OK
		: ts_store_prov_write_test(prov_blob, sizeof(prov_blob));

	printk("NB2 prov load=%d burn=%d\n", (int)lr, (int)wr);
	if (wr != TS_OK) {
		printk("NB FAIL prov\n");
		return 1;
	}
	(void)ts_safety_register_channel(&nb_led);
	(void)ts_hal_register_dev(&nb_dev);
	ts_core_boot(); /* noreturn：表尾 net_init → zenoh + 周期驱动 */
	return 0;
}
