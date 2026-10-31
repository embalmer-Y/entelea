/* SPDX-License-Identifier: Apache-2.0 */
/* otabench（单元 G2）：MCUmgr 固件 OTA 真机载体——MCUboot（sysbuild）+
 * SMP over UDP（WiFi）+ 真机 OTA 全链（v1 运行 → upload v2 → confirm →
 * reset → v2 运行）。
 * 判据（OTA* console 行 + PC 客户端 image list 双证据）：
 *   OTA0 版本自报（v1）；OTA1 WiFi IP；OTA2 SMP 服务在位
 *   （PC 侧：upload → image list 显 slot1=v2 → confirm → reset → 复位后
 *    OTA0 版本自报（v2）+ image list 显 running v2）
 * 无 zenoh（SMP 即本载体的传输面）；无 APP（OTA 证据 = 固件版本）。 */
#include <string.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <ts/core.h>
#include <ts/safety.h>
#include <ts/store.h>

/* WiFi 凭证经 cmake CACHE 变量注入（本地脚本，不入仓库） */
#ifndef OTAB_WIFI_SSID
#define OTAB_WIFI_SSID ""
#endif
#ifndef OTAB_WIFI_PASSWORD
#define OTAB_WIFI_PASSWORD ""
#endif

/* WiFi glue（netbench 板级七定稿模式——deploybench 同语义） */
static struct net_mgmt_event_callback ip_cb;
static struct net_mgmt_event_callback wifi_cb;
static atomic_t assoc_ok_evt;
static atomic_t disconn_evt;
static struct k_work_delayable wifi_retry_work;
static struct k_work assoc_work;
static struct wifi_connect_req_params cnx;
static struct k_sem ip_ready;

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
		net_dhcpv4_restart(iface);
		k_work_reschedule(&wifi_retry_work, K_SECONDS(2));
	}
	if (atomic_set(&assoc_ok_evt, 0) == 1) {
		struct wifi_ps_params ps = {.enabled = WIFI_PS_DISABLED};

		(void)net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
		printk("OTAw associated t=%u\n", (unsigned)k_uptime_get_32());
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
		const struct wifi_status *st = (const struct wifi_status *)cb->info;

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

/* prov：本地 OTA 载体缺省 ids（dev 缺省——本载体验证面 = 固件版本链） */
static void ota_alive(struct k_work *w)
{
	struct k_work_delayable *d = k_work_delayable_from_work(w);

	printk("OTA3 alive t=%u img-ver=%s\n", (unsigned)k_uptime_get_32(),
	       CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION);
	k_work_reschedule_for_queue(&k_sys_work_q, d, K_SECONDS(10));
}
static struct k_work_delayable ota_alive_work;

int main(void)
{
	printk("OTA0 otabench（单元 G2：MCUmgr OTA）img-ver=%s\n",
	       CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION);

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

	static const uint8_t ssid[] = OTAB_WIFI_SSID;
	static const uint8_t psk[] = OTAB_WIFI_PASSWORD;

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

	printk("OTA1 wifi_connect rc=%d\n", rc);
	if (k_sem_take(&ip_ready, K_SECONDS(40)) != 0) {
		printk("OTA FAIL dhcp timeout\n");
		return 1;
	}
	/* IPv4 地址自报（SMP UDP 客户端目标） */
	char buf[NET_IPV4_ADDR_LEN];

	if (net_addr_ntop(AF_INET,
			  &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
			  buf, sizeof(buf)) != NULL) {
		printk("OTA2 ip=%s smp-udp :1337\n", buf);
	}
	/* G2 收口修正（B2 后续）：smp_udp 在 SYS_INIT 早期（WiFi 关联前）
	 * 已启动——esp32 上 pre-IP 绑定的 UDP 套接字收不到后续到达的单播
	 * （线上铁证：请求出线 + ARP 通 + 板零回包）。拿到地址后重启
	 * smp_udp（close+open 重建套接字），绑定语义随 iface 就位。 */
	{
		extern int smp_udp_close(void);
		extern int smp_udp_open(void);
		int c = smp_udp_close();
		int o = smp_udp_open();

		printk("OTA2b smp_udp restart close=%d open=%d\n", c, o);
	}
	k_work_init_delayable(&ota_alive_work, ota_alive);
	k_work_reschedule_for_queue(&k_sys_work_q, &ota_alive_work, K_SECONDS(10));

	ts_core_boot(); /* noreturn：框架 boot 序（estop/poweron/wdt/core） */
	return 0;
}
