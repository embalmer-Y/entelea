/* SPDX-License-Identifier: Apache-2.0 */
/* F-1 真网闸：WiFi 关联 + DHCP + netmon 依赖联动真测（R06 合同
 * "真机批替换观测源"——dep_report 的观测源=真实 WiFi/DHCP 事件）。
 * 凭据=串口运行时注入（P4C1 协议=旧树 E1 实测纪律：不入镜像不入
 * 仓库；用后 mbedtls 风格 zeroize——本闸手写显式清零）。
 * 两轮（防抖真测）：connect→DHCP→dep good×2→UP→disconnect→
 * dep bad→DOWN（立即）；第二轮再 UP（DOWN→UP 需连续 2 次 good——
 * DEGRADED 中间态可见）。诊断失败停用网络依赖，不伪装成硬件安全故障。
 * 主线程阻塞等待安全：patrol 在 sysworkq（喂狗+通道重排）。 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <entelea/netmon.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"
#include "wifi_credentials.h"
#include "wifi_observer.h"
#include "../../../secure_zero.h"

#define CHK(cond, msg)                                                    \
	do {                                                              \
		bool ok_ = (cond);                                        \
		printk("[hw-wifi] %s: %s\n", ok_ ? "PASS" : "FAIL", msg); \
		if (!ok_) {                                               \
			goto fail;                                        \
		}                                                         \
	} while (0)

static entelea_clock_t platform_clock;

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback iface_cb;
static struct net_if *test_iface;
static struct entelea_wifi_observer wifi_observer;
static K_SEM_DEFINE(connected, 0, 1);
static K_SEM_DEFINE(disconnected, 0, 1);
static K_SEM_DEFINE(dhcp_bound, 0, 1);
static atomic_t connect_status;
static atomic_t disconnect_status;
static bool connect_requested;

static int
uart_read_exact(const struct device *uart, uint8_t *buf, size_t size,
		k_timepoint_t end)
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

struct credential_uart_reader {
	const struct device *uart;
	k_timepoint_t end;
};

static int
p4c1_uart_read(void *user, uint8_t *dst, size_t size)
{
	const struct credential_uart_reader *reader = user;

	return uart_read_exact(reader->uart, dst, size, reader->end);
}

static int
read_credentials(struct entelea_wifi_credentials *credentials)
{
	/* P4C1：magic + u8 ssid_len + u8 psk_len + ssid + psk（30s 窗）。 */
	struct credential_uart_reader reader = {
		.uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console)),
		.end = sys_timepoint_calc(K_SECONDS(30)),
	};

	if (!device_is_ready(reader.uart)) {
		return -ENODEV;
	}
	printk("[hw-wifi] CONFIG_WAIT (P4C1: ssid/psk over console)\n");
	return entelea_wifi_read_p4c1(credentials, p4c1_uart_read, &reader);
}

static void
network_event(struct net_mgmt_event_callback *cb, uint64_t event,
	      struct net_if *iface)
{
	const struct wifi_status *status;

	if (iface != test_iface) {
		return;
	}
	if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		entelea_wifi_observer_event(&wifi_observer,
					    ENTELEA_WIFI_DHCP_BOUND,
					    (uint64_t) k_uptime_get());
		k_sem_give(&dhcp_bound);
		return;
	}
	if (event == NET_EVENT_IPV4_DHCP_STOP ||
	    event == NET_EVENT_IPV4_ADDR_DEL) {
		entelea_wifi_observer_event(&wifi_observer,
					    ENTELEA_WIFI_DHCP_LOST,
					    (uint64_t) k_uptime_get());
		return;
	}
	if (event == NET_EVENT_IF_DOWN) {
		entelea_wifi_observer_event(&wifi_observer,
					    ENTELEA_WIFI_DISCONNECTED,
					    (uint64_t) k_uptime_get());
		return;
	}
	status =
		cb->info_length == sizeof(struct wifi_status) ? cb->info : NULL;
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		atomic_set(&connect_status, status ? status->status : -EIO);
		entelea_wifi_observer_event(&wifi_observer,
					    status && status->status == 0 ?
						    ENTELEA_WIFI_ASSOCIATED :
						    ENTELEA_WIFI_DISCONNECTED,
					    (uint64_t) k_uptime_get());
		k_sem_give(&connected);
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		atomic_set(&disconnect_status, status ? status->status : -EIO);
		entelea_wifi_observer_event(&wifi_observer,
					    ENTELEA_WIFI_DISCONNECTED,
					    (uint64_t) k_uptime_get());
		k_sem_give(&disconnected);
	}
}

static int
connect_and_report(const struct entelea_wifi_credentials *credentials,
		   int attempt)
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
	entelea_wifi_observer_event(&wifi_observer, ENTELEA_WIFI_DISCONNECTED,
				    (uint64_t) k_uptime_get());
	printk("[hw-wifi] CONNECT_BEGIN attempt=%d\n", attempt);
	connect_requested = true;
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, test_iface, &params,
		       sizeof(params));
	if (ret) {
		return ret;
	}
	ret = k_sem_take(&connected, K_SECONDS(30));
	if (ret) {
		return -ETIMEDOUT;
	}
	if (atomic_get(&connect_status)) {
		return -EIO;
	}
	printk("[hw-wifi] CONNECT_OK attempt=%d\n", attempt);
	net_dhcpv4_start(test_iface);
	ret = k_sem_take(&dhcp_bound, K_SECONDS(30));
	if (ret) {
		return -ETIMEDOUT;
	}
	if (!net_addr_ntop(NET_AF_INET, &test_iface->config.dhcpv4.requested_ip,
			   address, sizeof(address))) {
		return -EIO;
	}
	printk("[hw-wifi] DHCP_BOUND attempt=%d address=%s\n", attempt,
	       address);

	if (!entelea_wifi_observer_sample(&wifi_observer,
					  net_if_is_up(test_iface),
					  (uint64_t) k_uptime_get())) {
		return -EIO;
	}
	entelea_dep_state_t mid = entelea_dep_state(ENTELEA_DEP_NET);

	printk("[hw-wifi] dep NET after 1st good=%s\n",
	       mid == ENTELEA_DEP_DEGRADED ? "DEGRADED" :
	       mid == ENTELEA_DEP_UP	   ? "UP" :
					     "DOWN");
	k_sleep(K_MSEC(ENTELEA_NET_HB_PERIOD_MS));
	if (!entelea_wifi_observer_sample(&wifi_observer,
					  net_if_is_up(test_iface),
					  (uint64_t) k_uptime_get())) {
		return -EIO;
	}
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP) {
		return -EIO;
	}
	printk("[hw-wifi] dep NET UP (2 good, debounced)\n");
	return 0;
}

static int
disconnect_and_report(void)
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
	printk("[hw-wifi] DISCONNECT_OK\n");

	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DOWN) {
		return -EIO;
	}
	connect_requested = false;
	printk("[hw-wifi] dep NET DOWN (bad -> immediate)\n");
	return 0;
}

int
main(void)
{
	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK) {
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0;
	}
	printk("[hw-wifi] wifi/netmon gate begins\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);

	static struct entelea_wifi_credentials credentials = { 0 };
	int passed = 0;
	int ret;
	bool callbacks_registered = false;

	test_iface = net_if_get_default();
	CHK(test_iface && device_is_ready(net_if_get_device(test_iface)),
	    "default wifi iface ready (esp_hosted)");
	ret = net_if_up(test_iface);
	CHK(ret == 0 || ret == -EALREADY, "iface up");
	ret = read_credentials(&credentials);
	CHK(ret == 0, "credentials injected over console (P4C1)");
	net_mgmt_init_event_callback(&wifi_cb, network_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_init_event_callback(&ipv4_cb, network_event,
				     NET_EVENT_IPV4_DHCP_BOUND |
					     NET_EVENT_IPV4_DHCP_STOP |
					     NET_EVENT_IPV4_ADDR_DEL);
	net_mgmt_init_event_callback(&iface_cb, network_event,
				     NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_add_event_callback(&ipv4_cb);
	net_mgmt_add_event_callback(&iface_cb);
	callbacks_registered = true;
	for (int attempt = 1; attempt <= 2; attempt++) {
		ret = connect_and_report(&credentials, attempt);
		if (ret == 0) {
			ret = disconnect_and_report();
		}
		if (ret) {
			printk("[hw-wifi] FAIL attempt=%d rc=%d\n", attempt,
			       ret);
			break;
		}
		passed++;
		k_sleep(K_SECONDS(1));
	}
	CHK(passed == 2, "association+dhcp+dep-up/down x2/2");
	net_mgmt_del_event_callback(&wifi_cb);
	net_mgmt_del_event_callback(&ipv4_cb);
	net_mgmt_del_event_callback(&iface_cb);
	callbacks_registered = false;
	entelea_secure_zero(&credentials, sizeof(credentials));
	printk("[hw-wifi] PASS: wifi/netmon loop closed (net dep live)\n");
	k_sleep(K_FOREVER); /* 安全常驻（patrol 喂狗） */
	return 0;

fail:
	if (callbacks_registered && connect_requested) {
		int cleanup_ret = disconnect_and_report();

		if (cleanup_ret != 0) {
			net_dhcpv4_stop(test_iface);
			printk("[hw-wifi] disconnect unconfirmed rc=%d\n",
			       cleanup_ret);
		}
	}
	if (callbacks_registered) {
		net_mgmt_del_event_callback(&wifi_cb);
		net_mgmt_del_event_callback(&ipv4_cb);
		net_mgmt_del_event_callback(&iface_cb);
	}
	(void) entelea_dep_mark_down(ENTELEA_DEP_NET);
	entelea_secure_zero(&credentials, sizeof(credentials));
	printk("[hw-wifi] diagnostic stopped; network dependency DOWN\n");
	k_sleep(K_FOREVER);
	return 0;
}
