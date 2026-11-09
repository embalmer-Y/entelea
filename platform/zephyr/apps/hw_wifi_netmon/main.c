/* SPDX-License-Identifier: Apache-2.0 */
/* F-1 真网闸：WiFi 关联 + DHCP + netmon 依赖联动真测（R06 合同
 * "真机批替换观测源"——dep_report 的观测源=真实 WiFi/DHCP 事件）。
 * 凭据=串口运行时注入（P4C1 协议=旧树 E1 实测纪律：不入镜像不入
 * 仓库；用后 mbedtls 风格 zeroize——本闸手写显式清零）。
 * 两轮（防抖真测）：connect→DHCP→dep good×2→UP→disconnect→
 * dep bad→DOWN（立即）；第二轮再 UP（DOWN→UP 需连续 2 次 good——
 * DEGRADED 中间态可见）。任一环节失败 → system_fail fail-safe。
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

#define CHK(cond, msg) do { \
	bool ok_ = (cond); \
	printk("[hw-wifi] %s: %s\n", ok_ ? "PASS" : "FAIL", msg); \
	if (!ok_) { \
		goto fail; \
	} \
} while (0)

static entelea_clock_t platform_clock;

struct gate_credentials {
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

static int uart_read_exact(const struct device *uart, uint8_t *buf,
			   size_t size, k_timepoint_t end)
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

static int read_credentials(struct gate_credentials *credentials)
{
	/* P4C1：magic + u8 ssid_len + u8 psk_len + ssid + psk（30s 窗）。 */
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	k_timepoint_t end = sys_timepoint_calc(K_SECONDS(30));
	uint8_t header[6];
	int ret;

	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	printk("[hw-wifi] CONFIG_WAIT (P4C1: ssid/psk over console)\n");
	ret = uart_read_exact(uart, header, sizeof(header), end);
	if (ret) {
		return ret;
	}
	if (memcmp(header, "P4C1", 4) || !header[4] || header[4] > 32 ||
	    header[5] < 8 || header[5] > 63) {
		return -EINVAL;
	}
	credentials->ssid_length = header[4];
	credentials->psk_length = header[5];
	ret = uart_read_exact(uart, credentials->ssid,
			      credentials->ssid_length, end);
	if (ret) {
		return ret;
	}
	return uart_read_exact(uart, credentials->psk,
			       credentials->psk_length, end);
}

static void network_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
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
		/* P3-8（review）：命令化 disconnect 事件=命令响应；非命令性
		 * 掉链（AP 侧断）同经此事件——观测源真化：直连 dep bad
		 * （[thread] 面——mgmt 回调线程可调）。 */
		(void)entelea_dep_report(ENTELEA_DEP_NET, false, 0);
	}
}

static int connect_and_report(const struct gate_credentials *credentials,
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
	printk("[hw-wifi] CONNECT_BEGIN attempt=%d\n", attempt);
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
	if (!net_addr_ntop(NET_AF_INET,
			   &test_iface->config.dhcpv4.requested_ip,
			   address, sizeof(address))) {
		return -EIO;
	}
	printk("[hw-wifi] DHCP_BOUND attempt=%d address=%s\n", attempt,
	       address);

	/* 观测源实装：真实关联+DHCP 成功 = network 依赖 good。防抖（连续
	 * 2 次）→ UP；中间态 DEGRADED 在第 1 次后可见。 */
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	entelea_dep_state_t mid = entelea_dep_state(ENTELEA_DEP_NET);

	printk("[hw-wifi] dep NET after 1st good=%s\n",
	       mid == ENTELEA_DEP_DEGRADED ? "DEGRADED" :
	       mid == ENTELEA_DEP_UP ? "UP" : "DOWN");
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP) {
		return -EIO;
	}
	printk("[hw-wifi] dep NET UP (2 good, debounced)\n");
	return 0;
}

static int disconnect_and_report(void)
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

	/* 观测源实装：断链 = bad → DOWN 立即（失败侧保守）。 */
	(void)entelea_dep_report(ENTELEA_DEP_NET, false, 0);
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DOWN) {
		return -EIO;
	}
	printk("[hw-wifi] dep NET DOWN (bad -> immediate)\n");
	return 0;
}

int main(void)
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

	static struct gate_credentials credentials = {0};
	int passed = 0;
	int ret;
	static volatile uint8_t *cred_z =
		(volatile uint8_t *)&credentials; /* fail 路径同清（P3-1） */

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
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_add_event_callback(&ipv4_cb);
	for (int attempt = 1; attempt <= 2; attempt++) {
		ret = connect_and_report(&credentials, attempt);
		if (ret == 0) {
			ret = disconnect_and_report();
		}
		if (ret) {
			printk("[hw-wifi] FAIL attempt=%d rc=%d\n", attempt, ret);
			break;
		}
		passed++;
		k_sleep(K_SECONDS(1));
	}
	net_mgmt_del_event_callback(&wifi_cb);
	net_mgmt_del_event_callback(&ipv4_cb);
	/* 凭据显式清零（无 mbedtls 依赖的手写 zeroize）。 */
	for (size_t i = 0; i < sizeof(credentials); i++) {
		cred_z[i] = 0;
	}
	CHK(passed == 2, "association+dhcp+dep-up/down x2/2");
	printk("[hw-wifi] PASS: wifi/netmon loop closed (net dep live)\n");
	k_sleep(K_FOREVER); /* 安全常驻（patrol 喂狗） */
	return 0;

fail:
	for (size_t i = 0; i < sizeof(credentials); i++) {
		cred_z[i] = 0; /* 读中途失败的部分凭据同清（review P3-1） */
	}
	entelea_safety_system_fail(ENTELEA_FAIL_BOOT(7));
	return 0;
}
