// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <mbedtls/platform_util.h>
#include <string.h>
#include <ts/core.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>

#include "network.h"

struct test_credentials {
	uint8_t ssid[32];
	uint8_t psk[63];
	uint8_t ssid_length;
	uint8_t psk_length;
};

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct net_if *test_iface;
static K_SEM_DEFINE(connected, 0, 1);
static K_SEM_DEFINE(disconnected, 0, 1);
static K_SEM_DEFINE(dhcp_bound, 0, 1);
static atomic_t connect_status;
static atomic_t disconnect_status;

int p4_uart_read_exact(const struct device *uart, uint8_t *buf, size_t size, k_timepoint_t end)
{
	for (size_t i = 0; i < size;) {
		if (sys_timepoint_expired(end)) {
			return -ETIMEDOUT;
		}
		if (!uart_poll_in(uart, &buf[i])) {
			i++;
		} else {
			k_sleep(K_MSEC(1));
		}
	}
	return 0;
}

static int read_credentials(struct test_credentials *credentials)
{
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	k_timepoint_t end = sys_timepoint_calc(K_SECONDS(30));
	uint8_t header[6];
	int ret;

	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	printk("P4L CONFIG_WAIT\n");
	ret = p4_uart_read_exact(uart, header, sizeof(header), end);
	if (ret) {
		return ret;
	}
	if (memcmp(header, "P4C1", 4) || !header[4] || header[4] > 32 || header[5] < 8 ||
	    header[5] > 63) {
		return -EINVAL;
	}
	credentials->ssid_length = header[4];
	credentials->psk_length = header[5];
	ret = p4_uart_read_exact(uart, credentials->ssid, credentials->ssid_length, end);
	if (ret) {
		return ret;
	}
	return p4_uart_read_exact(uart, credentials->psk, credentials->psk_length, end);
}

static void network_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status;

	if (iface != test_iface) {
		return;
	}
	if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		k_sem_give(&dhcp_bound);
		return;
	}
	status = cb->info_length == sizeof(struct wifi_status) ? cb->info : NULL;
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		atomic_set(&connect_status, status ? status->status : -EIO);
		k_sem_give(&connected);
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		atomic_set(&disconnect_status, status ? status->status : -EIO);
		k_sem_give(&disconnected);
	}
}

static int connect_and_bind(const struct test_credentials *credentials, int attempt)
{
	char address[NET_IPV4_ADDR_LEN];
	struct wifi_connect_req_params params = {
		.ssid = credentials->ssid,
		.ssid_length = credentials->ssid_length,
		.psk = credentials->psk,
		.psk_length = credentials->psk_length,
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.timeout = 30,
	};
	int ret;

	k_sem_reset(&connected);
	k_sem_reset(&dhcp_bound);
	atomic_set(&connect_status, -EINPROGRESS);
	printk("P4L CONNECT_BEGIN attempt=%d\n", attempt);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, test_iface, &params, sizeof(params));
	if (ret) {
		return ret;
	}
	ret = k_sem_take(&connected, K_SECONDS(30));
	if (ret) {
		return -ETIMEDOUT;
	}
	ret = atomic_get(&connect_status);
	if (ret) {
		return -EIO;
	}
	printk("P4L CONNECT_OK attempt=%d\n", attempt);
	net_dhcpv4_start(test_iface);
	ret = k_sem_take(&dhcp_bound, K_SECONDS(30));
	if (ret) {
		return -ETIMEDOUT;
	}
	if (test_iface->config.dhcpv4.state != NET_DHCPV4_BOUND) {
		return -EIO;
	}
	if (!net_if_ipv4_addr_lookup_by_iface(test_iface,
					      &test_iface->config.dhcpv4.requested_ip) ||
	    !net_addr_ntop(NET_AF_INET, &test_iface->config.dhcpv4.requested_ip, address,
			   sizeof(address))) {
		return -EIO;
	}
	printk("P4L DHCP_BOUND attempt=%d\n", attempt);
	printk("P4L IPV4_READY address=%s\n", address);
	return 0;
}

static int disconnect_test(void)
{
	int ret;

	k_sem_reset(&disconnected);
	atomic_set(&disconnect_status, -EINPROGRESS);
	ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, test_iface, NULL, 0);
	if (ret) {
		return ret;
	}
	ret = k_sem_take(&disconnected, K_SECONDS(15));
	if (ret) {
		return -ETIMEDOUT;
	}
	if (atomic_get(&disconnect_status)) {
		return -EIO;
	}
	net_dhcpv4_stop(test_iface);
	printk("P4L DISCONNECT_OK\n");
	return 0;
}

int main(void)
{
	struct test_credentials credentials = {0};
	int passed = 0;
	int ret;

	test_iface = net_if_get_default();
	if (!test_iface || !device_is_ready(net_if_get_device(test_iface)) ||
	    ts_wdt_start() != TS_OK) {
		printk("P4L FAIL device/watchdog not ready\n");
		return 1;
	}
	ret = net_if_up(test_iface);
	if (ret && ret != -EALREADY) {
		printk("P4L FAIL interface up rc=%d\n", ret);
		return 1;
	}
	ret = read_credentials(&credentials);
	if (ret) {
		printk("P4L FAIL credential input rc=%d\n", ret);
		goto out;
	}
	printk("P4L CONFIG_READY security=wpa2-psk\n");
	net_mgmt_init_event_callback(&wifi_cb, network_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_init_event_callback(&ipv4_cb, network_event, NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_add_event_callback(&ipv4_cb);
	for (int attempt = 1; attempt <= 2; attempt++) {
		int disconnect_ret;

		ret = connect_and_bind(&credentials, attempt);
		if (!ret) {
			if (IS_ENABLED(CONFIG_P4W_NETWORK_TLS)) {
				ret = p4_tls_test();
			}
			disconnect_ret = disconnect_test();
			if (disconnect_ret) {
				printk("P4L FAIL disconnect cleanup rc=%d\n", disconnect_ret);
				if (!ret) {
					ret = disconnect_ret;
				}
			}
		}
		if (ret) {
			printk("P4L FAIL attempt=%d rc=%d\n", attempt, ret);
			break;
		}
		passed++;
		k_sleep(K_SECONDS(1));
	}
	net_mgmt_del_event_callback(&wifi_cb);
	net_mgmt_del_event_callback(&ipv4_cb);
out:
	mbedtls_platform_zeroize(&credentials, sizeof(credentials));
	printk("P4L %s association_dhcp_disconnect=%d/2\n", passed == 2 ? "PASS" : "FAIL", passed);
	return passed == 2 ? 0 : 1;
}
