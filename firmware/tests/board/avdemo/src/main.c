/* SPDX-License-Identifier: Apache-2.0 */
/* avdemo（MD1.2h）：D-AV demo 真机载体——ts-av natives 全链（DEC-47③⑥）。
 * 流程：WiFi STA（deploybench 同款 glue）→ prov flash 持久 → 安装 D-AV 包
 * （机械生成头文件）→ ts_core_boot（net_init → zenoh 会话 + 步骤 8 装载）→
 * APP 状态机：ts_av_capture 取 JPEG 帧 → 切 ≤1KB 分片 → ts_net_publish
 * （avq：宿主打拍/重试）→ PC 侧 dav_recv.py 重组校验（独立证据）。
 * 判据（DA* console 行 + PC 侧）：DA1 安装 → DA2 会话 → DA3 ACTIVE →
 * DA4 存活观测（含 avq dropped）→ PC：≥10 完整帧（crc32 对 + JPEG SOI/EOI）
 * → DA PASS。 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h> /* 须在 net_if.h 之后 */
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <ts/appmgr.h>
#include <ts/core.h>
#include <ts/net.h>
#include <ts/store.h>

#include "dav_pkg.h"

#ifndef AVD_WIFI_SSID
#define AVD_WIFI_SSID ""
#endif
#ifndef AVD_WIFI_PASSWORD
#define AVD_WIFI_PASSWORD ""
#endif

/* ---- WiFi glue（netbench 板级七定稿模式，deploybench 全文同语义）---------- */

static struct net_mgmt_event_callback ip_cb;
static struct net_mgmt_event_callback wifi_cb;
static struct k_sem ip_ready;
static atomic_t assoc_ok_evt;
static atomic_t disconn_evt;
static struct k_work_delayable wifi_retry_work;
static struct k_work assoc_work;
static struct wifi_connect_req_params cnx;

static void wifi_try_connect(struct k_work *w)
{
	ARG_UNUSED(w);
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, net_if_get_default(),
			  &cnx, sizeof(cnx));

	if (rc != 0) {
		k_work_reschedule(&wifi_retry_work, K_SECONDS(2));
	}
}

static void wifi_assoc_work(struct k_work *w)
{
	ARG_UNUSED(w);
	struct net_if *iface = net_if_get_default();

	if (atomic_set(&disconn_evt, 0) == 1) {
		ts_net_session_media_down();
		net_dhcpv4_restart(iface);
		k_work_reschedule(&wifi_retry_work, K_SECONDS(2));
	}
	if (atomic_set(&assoc_ok_evt, 0) == 1) {
		struct wifi_ps_params ps = {.enabled = WIFI_PS_DISABLED};

		(void)net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
		printk("DAw associated t=%u\n", (unsigned)k_uptime_get_32());
	}
}

static void ip_handler(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (event == NET_EVENT_IPV4_ADDR_ADD && net_if_is_up(iface)) {
		k_sem_give(&ip_ready);
	}
}

static void wifi_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;

		if (st != NULL && st->status == 0) {
			atomic_set(&assoc_ok_evt, 1);
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		atomic_set(&disconn_evt, 1);
	}
	if (atomic_get(&assoc_ok_evt) == 1 || atomic_get(&disconn_evt) == 1) {
		k_work_submit(&assoc_work);
	}
}

/* {"v":1,"node_id":"dav","cube_id":"dvc","routers":["tcp/192.168.2.90:9955"],
 *  …}（deploybench prov_blob 脚本机械改 id 生成并走查验证——gen_dav_prov.py；
 * 手抄二进制数组禁令，教训 27）。 */
static const uint8_t prov_blob[] = {
	0xa9, 0x61, 0x76, 0x01, 0x67, 0x6e, 0x6f, 0x64, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x64, 0x61, 0x76, 0x67, 0x63, 0x75, 0x62, 0x65, 0x5f, 0x69, 0x64,
	0x63, 0x64, 0x76, 0x63, 0x67, 0x72, 0x6f, 0x75, 0x74, 0x65, 0x72, 0x73,
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

static const uint8_t root_pub[32]; /* TEST 语义验签（结构检查；Q-29 批收口） */

/* ---- 装载观测线程：会话 → ACTIVE → avq 观测 ------------------------------ */

static void dav_watch(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (int i = 0; i < 600 && ts_net_state() != TS_NET_CONNECTED; i++) {
		k_msleep(100);
	}
	if (ts_net_state() != TS_NET_CONNECTED) {
		printk("DA FAIL no session t=%u\n", (unsigned)k_uptime_get_32());
		return;
	}
	printk("DA2 session CONNECTED t=%u\n", (unsigned)k_uptime_get_32());

	ts_app_info_t info;

	for (int i = 0; i < 300; i++) {
		if (ts_appmgr_get_info(&info) == TS_OK &&
		    info.state == TS_APP_ACTIVE) {
			break;
		}
		k_msleep(200);
	}
	if (info.state != TS_APP_ACTIVE) {
		printk("DA FAIL no ACTIVE\n");
		return;
	}
	printk("DA3 APP ACTIVE app=%s\n", info.app_id);

	/* APP 自报进度经 ts_log_write（[app1/l*] 行）；宿主侧独立观测 =
	 * avq 丢弃计数 + APP 状态（健康面）。帧内容判定在 PC 侧（独立证据）。 */
	uint32_t drop0 = ts_net_avq_dropped();

	for (int i = 0;; i++) {
		k_msleep(5000);
		ts_app_info_t cur;

		(void)ts_appmgr_get_info(&cur);
		printk("DA4 alive t=%u net=%d state=%d dropped=%u\n",
		       (unsigned)k_uptime_get_32(), (int)ts_net_state(),
		       (int)cur.state, ts_net_avq_dropped() - drop0);
	}
}
K_THREAD_DEFINE(dav_watch_tid, 4096, dav_watch, NULL, NULL, NULL, 13, 0, 0);

int main(void)
{
	printk("DA0 avdemo（MD1.2h：D-AV demo ts-av 全链）\n");

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

	static const uint8_t ssid[] = AVD_WIFI_SSID;
	static const uint8_t psk[] = AVD_WIFI_PASSWORD;

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

	printk("DA1 wifi_connect rc=%d\n", rc);
	if (k_sem_take(&ip_ready, K_SECONDS(40)) != 0) {
		printk("DA FAIL dhcp timeout\n");
		return 1;
	}

	ts_res_t lr = ts_store_prov_load();
	ts_res_t wr = (lr == TS_OK)
		? TS_OK
		: ts_store_prov_write_test(prov_blob, sizeof(prov_blob));

	printk("DA1b prov load=%d burn=%d\n", (int)lr, (int)wr);
	if (wr != TS_OK) {
		printk("DA FAIL prov\n");
		return 1;
	}

	ts_app_info_t info;
	ts_res_t ir = ts_appmgr_install(dav_pkg, sizeof(dav_pkg), root_pub, &info);

	printk("DA1c install r=%d state=%d slot=%u\n",
	       (int)ir, (int)info.state, info.active_slot);
	if (ir != TS_OK) {
		printk("DA FAIL install\n");
		return 1;
	}
	ts_core_boot(); /* noreturn：net_init → zenoh 会话 + 步骤 8 装载 APP */
	return 0;
}
