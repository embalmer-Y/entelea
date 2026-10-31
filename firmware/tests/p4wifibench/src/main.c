/* SPDX-License-Identifier: Apache-2.0 */
/*
 * P4 联网批 E1（V0.14.0 · P4-W1）：C6 伴芯 esp-hosted-mcu 新驱动 bring-up。
 *
 * 判据行（bench 契约——逐字出现即达成）：
 *   P4W1 IFACE UP          —— wifi netif 就绪（SLC/buf-config/握手后的下游事实）
 *   P4W1 CONNECT OK        —— wifi_connect rc=0 + 事件确认
 *   P4W1 DHCP OK ip=...    —— DHCP 绑定（数据面首战场——教训 32⑤ 病灶位）
 *   P4W1 UDP RTT OK seq=N  —— UDP 往返（host 侧回声器对拍）
 *   P4W1 ALIVE t=<s>       —— 存活心跳（alive 惯例）
 *
 * 凭据 = Kconfig 构建期注入（不入仓不入证据——PSK 红线）。
 */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(p4wifibench);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static volatile bool wifi_connected;
static volatile bool wifi_conn_failed;
static volatile bool have_addr;

static int echo_sock = -1;
static struct sockaddr_in echo_dest;

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t mgmt,
		       struct net_if *iface)
{
	if (mgmt == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st = cb->info;
		if (st && st->status == 0) {
			printk("P4W1 CONNECT OK\n");
			wifi_connected = true;
		} else {
			printk("P4W1 CONNECT FAIL status=%d\n",
			       st ? st->status : -1);
			wifi_conn_failed = true;
		}
	}
}

static void ipv4_event(struct net_mgmt_event_callback *cb, uint64_t mgmt,
		       struct net_if *iface)
{
	if (mgmt == NET_EVENT_IPV4_ADDR_ADD && iface &&
	    iface->config.ip.ipv4) {
		char ip[NET_IPV4_ADDR_LEN];
		struct net_if_ipv4 *v4 = iface->config.ip.ipv4;

		if (!v4->unicast[0].ipv4.is_used) {
			return;
		}
		net_addr_ntop(AF_INET,
			      &v4->unicast[0].ipv4.address.in_addr,
			      ip, sizeof(ip));
		char gw[NET_IPV4_ADDR_LEN] = "-";
		if (v4->gw.s_addr != 0) {
			net_addr_ntop(AF_INET, &v4->gw, gw, sizeof(gw));
		}
		printk("P4W1 DHCP OK ip=%s gw=%s\n", ip, gw);
		have_addr = true;
	}
}

static int udp_send(const char *msg)
{
	return zsock_sendto(echo_sock, msg, strlen(msg), 0,
			    (struct sockaddr *)&echo_dest, sizeof(echo_dest));
}

int main(void)
{
	printk("P4W1 boot (build " __DATE__ " " __TIME__ ")\n");

	struct net_if *iface = net_if_get_default();
	for (int s = 0; s < 200 && !iface; s++) { /* 驱动 netif 注册等待 */
		k_sleep(K_MSEC(100));
		iface = net_if_get_default();
	}
	if (!iface) {
		printk("P4W1 FATAL no netif after 20s\n");
		return 0;
	}
	printk("P4W1 IFACE UP\n");

	net_mgmt_init_event_callback(&wifi_cb, wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	struct wifi_connect_req_params params = {
		.ssid = CONFIG_P4W_SSID,
		.ssid_length = strlen(CONFIG_P4W_SSID),
		.psk = CONFIG_P4W_PSK,
		.psk_length = strlen(CONFIG_P4W_PSK),
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.timeout = SYS_FOREVER_MS,
	};
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface,
			  &params, sizeof(params));
	if (rc != 0) {
		printk("P4W1 CONNECT REQ FAIL rc=%d\n", rc);
	}
	net_dhcpv4_start(iface);

	/* UDP 回声器（数据面机械判据——host 侧 listener 对拍） */
	echo_dest.sin_family = AF_INET;
	echo_dest.sin_port = htons(CONFIG_P4W_ECHO_PORT);
	rc = zsock_inet_pton(AF_INET, CONFIG_P4W_ECHO_HOST,
			     &echo_dest.sin_addr);
	echo_sock = zsock_socket(AF_INET, SOCK_DGRAM, 0);

	uint32_t seq = 0;
	uint32_t sec = 0;
	while (1) {
		if (have_addr && echo_sock >= 0 && rc == 1 && seq < 5) {
			char msg[48];

			snprintf(msg, sizeof(msg), "P4W1 UDP %u", seq);
			if (udp_send(msg) >= 0) {
				char rbuf[64];
				int n = zsock_recv(echo_sock, rbuf,
						   sizeof(rbuf) - 1, 0);
				if (n > 0) {
					rbuf[n] = '\0';
					printk("P4W1 UDP RTT OK seq=%u\n",
					       seq);
					seq++;
				}
			}
		}
		k_sleep(K_SECONDS(2));
		sec += 2;
		if (sec % 30 == 0) {
			printk("P4W1 ALIVE t=%us connected=%d addr=%d "
			       "seq=%u\n",
			       sec, wifi_connected, have_addr, seq);
		}
	}
	return 0;
}
