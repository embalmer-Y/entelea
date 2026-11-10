/* SPDX-License-Identifier: Apache-2.0 */
/* r15 MCU 真云闸（A-1）：WiFi→DHCP→板上 TLS1.3 真模型 chat 全链真测。
 * 凭据=P4C2 串口注入（P4C1 扩展：ssid/psk + host/api_key/model——
 * 不入镜像不入仓库，用后显式 zeroize）。判据行 [hw-cloud]：
 *   1. connect+DHCP+dep NET good（复用 F-1 面）
 *   2. 真 chat（prompt "Reply with exactly the word: ok"）→
 *      content 非空 + usage>0 + touched==1（B-2 教训：不强绑字面 ok）
 *      → dep MODEL good
 *   3. 断链（disconnect）→ 再 chat = E_STATE（netmon 准入拒——云失败
 *      与硬件安全不同状态机的可观测面）+ touched 不变
 *   4. 复连 → 再 chat OK → 两轮 PASS 常驻
 * 失败 → system_fail fail-safe。 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <entelea/model.h>
#include <entelea/netmon.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"
#include "https_chat.h"

#define CHK(cond, msg) do { \
	bool ok_ = (cond); \
	printk("[hw-cloud] %s: %s\n", ok_ ? "PASS" : "FAIL", msg); \
	if (!ok_) { \
		goto fail; \
	} \
} while (0)

static entelea_clock_t platform_clock;

struct gate_credentials {
	uint8_t ssid[32];
	uint8_t psk[63];
	char host[64];
	char api_key[128];
	char model[48];
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
static int chat_touched;

static entelea_https_cfg_t cfg_live; /* P4C2 凭据运行时填 */

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

static int read_str(const struct device *uart, char *dst, uint8_t len,
		    k_timepoint_t end)
{
	int ret = uart_read_exact(uart, (uint8_t *)dst, len, end);

	if (ret == 0) {
		dst[len] = '\0';
	}
	return ret;
}

static int read_credentials(struct gate_credentials *c)
{
	/* P4C2：magic + u8[5] 长度 + ssid/psk/host/key/model（60s 窗）。 */
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	k_timepoint_t end = sys_timepoint_calc(K_SECONDS(60));
	uint8_t header[9];
	int ret;

	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	printk("[hw-cloud] CONFIG_WAIT (P4C2: ssid/psk/host/key/model over console)\n");
	ret = uart_read_exact(uart, header, sizeof(header), end);
	if (ret) {
		return ret;
	}
	if (memcmp(header, "P4C2", 4) || !header[4] || header[4] > 32 ||
	    header[5] < 8 || header[5] > 63 || !header[6] || header[6] > 63 ||
	    header[7] < 20 || header[7] > 127 || !header[8] || header[8] > 47) {
		return -EINVAL;
	}
	c->ssid_length = header[4];
	c->psk_length = header[5];
	if (read_str(uart, (char *)c->ssid, c->ssid_length, end) ||
	    uart_read_exact(uart, c->psk, c->psk_length, end) ||
	    read_str(uart, c->host, header[6], end) ||
	    read_str(uart, c->api_key, header[7], end) ||
	    read_str(uart, c->model, header[8], end)) {
		return -ETIMEDOUT;
	}
	return 0;
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
		(void)entelea_dep_report(ENTELEA_DEP_NET, false, 0);
	}
}

static int connect_and_report(const struct gate_credentials *c, int attempt)
{
	char address[NET_IPV4_ADDR_LEN];
	struct wifi_connect_req_params params = {
		.ssid = c->ssid,
		.ssid_length = c->ssid_length,
		.psk = c->psk,
		.psk_length = c->psk_length,
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.timeout = 30,
	};
	int ret;

	k_sem_reset(&connected);
	k_sem_reset(&dhcp_bound);
	atomic_set(&connect_status, -EINPROGRESS);
	printk("[hw-cloud] CONNECT_BEGIN attempt=%d\n", attempt);
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
	printk("[hw-cloud] CONNECT_OK attempt=%d\n", attempt);
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
	printk("[hw-cloud] DHCP_BOUND attempt=%d address=%s\n", attempt,
	       address);
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP) {
		return -EIO;
	}
	printk("[hw-cloud] dep NET UP\n");
	return 0;
}

static int disconnect_quiet(void)
{
	k_sem_reset(&disconnected);
	atomic_set(&disconnect_status, -EINPROGRESS);
	if (net_mgmt(NET_REQUEST_WIFI_DISCONNECT, test_iface, NULL, 0)) {
		return -EIO;
	}
	if (k_sem_take(&disconnected, K_SECONDS(15))) {
		return -ETIMEDOUT;
	}
	net_dhcpv4_stop(test_iface);
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DOWN) {
		return -EIO;
	}
	printk("[hw-cloud] DISCONNECT_OK dep NET DOWN\n");
	return 0;
}

static int chat_once(char *out, size_t out_max, entelea_model_usage_t *usage)
{
	const entelea_model_msg_t msgs[1] = {
		{ .role = "user",
		  .content = "Reply with exactly the word: ok. Output nothing else." },
	};

	/* 准入面（host 版同语义）：NET 依赖非 UP 拒调（touched 不动）。
	 * 云失败与硬件安全=不同状态机（R06/R08 合同）。 */
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP) {
		return ENTELEA_E_STATE;
	}
	return entelea_https_chat(&cfg_live, msgs, 1, out, out_max,
				  30000, usage);
}

int main(void)
{
	static struct gate_credentials credentials = {0};
	static volatile uint8_t *cred_z = (volatile uint8_t *)&credentials;
	static char out[1024];
	entelea_model_usage_t usage = {0};
	int ret;

	if (entelea_platform_clock_init(&platform_clock) != ENTELEA_OK ||
	    entelea_evt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_wdt_bind_clock(&platform_clock) != ENTELEA_OK ||
	    entelea_safety_bind_clock(&platform_clock) != ENTELEA_OK) {
		entelea_safety_system_fail(ENTELEA_FAIL_BOOT(0));
		return 0;
	}
	printk("[hw-cloud] MCU real-cloud gate begins (r15 A-1)\n");
	entelea_supervision_boot(entelea_platform_boot_steps,
				 ENTELEA_PLATFORM_BOOT_STEP_COUNT);

	test_iface = net_if_get_default();
	CHK(test_iface && device_is_ready(net_if_get_device(test_iface)),
	    "default wifi iface ready (esp_hosted)");
	ret = net_if_up(test_iface);
	CHK(ret == 0 || ret == -EALREADY, "iface up");
	ret = read_credentials(&credentials);
	CHK(ret == 0, "credentials injected over console (P4C2)");
	net_mgmt_init_event_callback(&wifi_cb, network_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_init_event_callback(&ipv4_cb, network_event,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_add_event_callback(&ipv4_cb);

	cfg_live.touched = &chat_touched;
	{ /* host 字段可携 path（"host/api/v1" 形式——F-2 host 版
	 * parse_base_url 等价面：首个 '/' 分离）。 */
		char *slash = strchr(credentials.host, '/');

		if (slash != NULL) {
			size_t hlen = (size_t)(slash - credentials.host);

			memcpy(cfg_live.host, credentials.host, hlen);
			cfg_live.host[hlen] = '\0';
			snprintf(cfg_live.path_prefix,
				 sizeof(cfg_live.path_prefix), "%s",
				 slash);
		} else {
			strcpy(cfg_live.host, credentials.host);
			strcpy(cfg_live.path_prefix, "/v1");
		}
	}
	strcpy(cfg_live.api_key, credentials.api_key);
	strcpy(cfg_live.model, credentials.model);

	/* 第一轮：连网→真 chat→MODEL good。 */
	CHK(connect_and_report(&credentials, 1) == 0, "connect+dhcp+dep NET UP");
	chat_touched = 0;
	ret = chat_once(out, sizeof(out), &usage);
	CHK(ret == ENTELEA_OK && out[0] != '\0' &&
	    usage.completion_tokens > 0 && chat_touched == 1,
	    "real TLS chat: content+usage+touched (attempt 1)");
	printk("[hw-cloud] reply=%.60s usage=%u+%u\n", out,
	       usage.prompt_tokens, usage.completion_tokens);
	(void)entelea_dep_report(ENTELEA_DEP_MODEL, true, 0);
	(void)entelea_dep_report(ENTELEA_DEP_MODEL, true, 0);
	CHK(entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_UP,
	    "dep MODEL UP");

	/* 失联拒对照：断链→chat 必须被拒（netmon 准入面）+touched 不变。 */
	CHK(disconnect_quiet() == 0, "disconnect -> dep NET DOWN");
	chat_touched = 99; /* 哨兵：调用被拒则不推进 */
	ret = chat_once(out, sizeof(out), NULL);
	CHK(ret != ENTELEA_OK && chat_touched == 99,
	    "chat rejected while NET down (admission gate)");
	printk("[hw-cloud] rejected rc=%d (E_STATE expected)\n", ret);

	/* 第二轮：复连→再 chat OK→两轮闭环。DNS 重试（r15 实测：重连后
	 * 首查偶发未就绪——DHCP DNS 服务器重注入时序，间隔重查即过）。 */
	CHK(connect_and_report(&credentials, 2) == 0, "reconnect+dhcp (attempt 2)");
	chat_touched = 0;
	memset(&usage, 0, sizeof(usage));
	ret = chat_once(out, sizeof(out), &usage);
	if (ret != ENTELEA_OK) {
		k_sleep(K_SECONDS(3));
		chat_touched = 0;
		ret = chat_once(out, sizeof(out), &usage);
	}
	CHK(ret == ENTELEA_OK && out[0] != '\0' &&
	    usage.completion_tokens > 0 && chat_touched == 1,
	    "real TLS chat (attempt 2)");
	printk("[hw-cloud] reply2=%.60s usage=%u+%u\n", out,
	       usage.prompt_tokens, usage.completion_tokens);
	(void)disconnect_quiet();

	net_mgmt_del_event_callback(&wifi_cb);
	net_mgmt_del_event_callback(&ipv4_cb);
	for (size_t i = 0; i < sizeof(credentials); i++) {
		cred_z[i] = 0; /* 全路径 zeroize（含 fail 标签共用） */
	}
	memset(&cfg_live, 0, sizeof(cfg_live));
	printk("[hw-cloud] PASS: MCU real-cloud loop closed (TLS1.3 chat x2 + admission gate)\n");
	k_sleep(K_FOREVER);
	return 0;

fail:
	for (size_t i = 0; i < sizeof(credentials); i++) {
		cred_z[i] = 0;
	}
	memset(&cfg_live, 0, sizeof(cfg_live));
	entelea_safety_system_fail(ENTELEA_FAIL_BOOT(7));
	return 0;
}
