/* SPDX-License-Identifier: Apache-2.0 */
/* r15 MCU real-cloud gate (A-1): full-chain real test of WiFi -> DHCP ->
 * on-board TLS 1.3 real model chat.
 * Credentials are injected over the P4C2 serial port (P4C1 extension:
 * ssid/psk + host/api_key/model). They are not stored in the image or repo,
 * and are zeroized explicitly after use. Criteria lines [hw-cloud]:
 *   1. connect+DHCP+dep NET good (reuses the F-1 surface)
 *   2. one real recovery probe -> MODEL DEGRADED (one observation)
 *   3. link down (disconnect) -> chat again = E_STATE (rejected by netmon
 *      admission; cloud failure is observed on a state machine distinct from
 *      hardware safety) + touched unchanged
 *   4. reconnect -> second real recovery probe -> MODEL UP -> ordinary chat
 * A diagnostic failure disables only the cloud path and this diagnostic; real
 * boot or supervision faults still take the safe-stop path. */
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
#include <entelea/d12.h>
#include <entelea/model.h>
#include <entelea/netmon.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"
#include "https_chat.h"
#include "p4c2.h"
#include "wifi_observer.h"
#include "../../../secure_zero.h"

#define CHK(cond, msg)                                                     \
	do {                                                               \
		bool ok_ = (cond);                                         \
		printk("[hw-cloud] %s: %s\n", ok_ ? "PASS" : "FAIL", msg); \
		if (!ok_) {                                                \
			goto fail;                                         \
		}                                                          \
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
static int chat_touched;
static bool connect_requested;

static entelea_https_cfg_t cfg_live; /* runtime-filled from P4C2 creds */

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
p4c2_uart_read(void *user, uint8_t *dst, size_t size)
{
	const struct credential_uart_reader *reader = user;

	return uart_read_exact(reader->uart, dst, size, reader->end);
}

static int
read_credentials(struct gate_credentials *c)
{
	/* P4C2: magic, u8[5] lengths, ssid/psk/host/key/model (60s window). */
	struct credential_uart_reader reader = {
		.uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console)),
		.end = sys_timepoint_calc(K_SECONDS(60)),
	};

	if (!device_is_ready(reader.uart)) {
		return -ENODEV;
	}
	printk("[hw-cloud] CONFIG_WAIT (P4C2: ssid/psk/host/key/model over "
	       "console)\n");
	return entelea_p4c2_read(c, p4c2_uart_read, &reader);
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
connect_and_report(const struct gate_credentials *c, int attempt)
{
	char address[NET_IPV4_ADDR_LEN];
	struct wifi_connect_req_params params = {
		.ssid = c->wifi.ssid,
		.ssid_length = c->wifi.ssid_length,
		.psk = c->wifi.psk,
		.psk_length = c->wifi.psk_length,
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
	printk("[hw-cloud] CONNECT_BEGIN attempt=%d\n", attempt);
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
	printk("[hw-cloud] CONNECT_OK attempt=%d\n", attempt);
	net_dhcpv4_start(test_iface);
	ret = k_sem_take(&dhcp_bound, K_SECONDS(30));
	if (ret) {
		return -ETIMEDOUT;
	}
	if (!net_addr_ntop(NET_AF_INET, &test_iface->config.dhcpv4.requested_ip,
			   address, sizeof(address))) {
		return -EIO;
	}
	printk("[hw-cloud] DHCP_BOUND attempt=%d address=%s\n", attempt,
	       address);
	for (int i = 0; i < 2; i++) {
		if (!entelea_wifi_observer_sample(&wifi_observer,
						  net_if_is_up(test_iface),
						  (uint64_t) k_uptime_get())) {
			return -EIO;
		}
		if (i == 0) {
			k_sleep(K_MSEC(ENTELEA_NET_HB_PERIOD_MS));
		}
	}
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP) {
		return -EIO;
	}
	printk("[hw-cloud] dep NET UP\n");
	return 0;
}

static int
disconnect_quiet(void)
{
	k_sem_reset(&disconnected);
	atomic_set(&disconnect_status, -EINPROGRESS);
	if (net_mgmt(NET_REQUEST_WIFI_DISCONNECT, test_iface, NULL, 0)) {
		return -EIO;
	}
	if (k_sem_take(&disconnected, K_SECONDS(15))) {
		return -ETIMEDOUT;
	}
	if (atomic_get(&disconnect_status)) {
		return -EIO;
	}
	net_dhcpv4_stop(test_iface);
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DOWN) {
		return -EIO;
	}
	connect_requested = false;
	printk("[hw-cloud] DISCONNECT_OK dep NET DOWN\n");
	return 0;
}

static int
chat_once(bool probe, char *out, size_t out_max, entelea_model_usage_t *usage)
{
	const entelea_model_msg_t msgs[1] = {
		{ .role = "user",
		  .content = "Reply with exactly the word: ok. Output nothing "
			     "else." },
	};

	entelea_model_usage_t before = entelea_model_usage_total();
	entelea_model_usage_t after = before;
	int touched_before = chat_touched;
	uint32_t calls_before = entelea_d12_model_calls();
	entelea_result_t ret =
		probe ? entelea_model_probe(msgs, 1, out, out_max, 30000,
					    &after) :
			entelea_model_ask(msgs, 1, out, out_max, 30000, &after);

	if (usage != NULL) {
		usage->prompt_tokens =
			after.prompt_tokens - before.prompt_tokens;
		usage->completion_tokens =
			after.completion_tokens - before.completion_tokens;
	}
	if (chat_touched - touched_before !=
	    (int) (entelea_d12_model_calls() - calls_before)) {
		return ENTELEA_E_STATE;
	}
	if (chat_touched > touched_before) {
		(void) entelea_dep_report(ENTELEA_DEP_MODEL,
					  ret == ENTELEA_OK && out[0] != '\0',
					  (uint64_t) k_uptime_get());
	}
	return ret;
}

int
main(void)
{
	static struct gate_credentials credentials = { 0 };
	static char out[1024];
	entelea_model_usage_t usage = { 0 };
	bool callbacks_registered = false;
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
	if (entelea_platform_d12_safe_halt()) {
		printk("[hw-cloud] D12 SAFE-HALT: cloud disabled\n");
		k_sleep(K_FOREVER);
		return 0;
	}

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
				     NET_EVENT_IPV4_DHCP_BOUND |
					     NET_EVENT_IPV4_DHCP_STOP |
					     NET_EVENT_IPV4_ADDR_DEL);
	net_mgmt_init_event_callback(&iface_cb, network_event,
				     NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_add_event_callback(&ipv4_cb);
	net_mgmt_add_event_callback(&iface_cb);
	callbacks_registered = true;

	cfg_live.touched = &chat_touched;
	{ /* The host field may carry a path ("host/api/v1" form), matching the
	 * F-2 host version's parse_base_url: split at the first '/'. */
		char *slash = strchr(credentials.host, '/');

		if (slash != NULL) {
			size_t hlen = (size_t) (slash - credentials.host);

			memcpy(cfg_live.host, credentials.host, hlen);
			cfg_live.host[hlen] = '\0';
			snprintf(cfg_live.path_prefix,
				 sizeof(cfg_live.path_prefix), "%s", slash);
		} else {
			strcpy(cfg_live.host, credentials.host);
			strcpy(cfg_live.path_prefix, "/v1");
		}
	}
	strcpy(cfg_live.api_key, credentials.api_key);
	strcpy(cfg_live.model, credentials.model);
	static const entelea_model_transport_t transport = {
		.chat = entelea_https_chat,
	};

	entelea_model_bind(&transport, &cfg_live);

	/* Round 1: one real probe yields one MODEL good observation. */
	CHK(connect_and_report(&credentials, 1) == 0,
	    "connect+dhcp+dep NET UP");
	chat_touched = 0;
	ret = chat_once(true, out, sizeof(out), &usage);
	CHK(ret == ENTELEA_OK && out[0] != '\0' &&
		    usage.completion_tokens > 0 && chat_touched >= 1,
	    "first gated MODEL recovery probe");
	printk("[hw-cloud] reply=%.60s usage=%u+%u\n", out, usage.prompt_tokens,
	       usage.completion_tokens);
	CHK(entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_DEGRADED,
	    "one successful probe leaves MODEL DEGRADED");

	/* Negative check: with the link down, chat must be rejected by the
	 * netmon admission gate and touched must not change. */
	CHK(disconnect_quiet() == 0, "disconnect -> dep NET DOWN");
	chat_touched = 99; /* sentinel: unchanged if the call is rejected */
	ret = chat_once(false, out, sizeof(out), NULL);
	CHK(ret != ENTELEA_OK && chat_touched == 99,
	    "chat rejected while NET down (admission gate)");
	printk("[hw-cloud] rejected rc=%d (E_STATE expected)\n", ret);

	/* Ordinary calls are allowed only after a second real success. */
	CHK(connect_and_report(&credentials, 2) == 0,
	    "reconnect+dhcp (attempt 2)");
	chat_touched = 0;
	memset(&usage, 0, sizeof(usage));
	ret = chat_once(true, out, sizeof(out), &usage);
	CHK(ret == ENTELEA_OK && out[0] != '\0' &&
		    usage.completion_tokens > 0 && chat_touched >= 1,
	    "second gated MODEL recovery probe");
	CHK(entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_UP,
	    "two successful probes restore MODEL UP");
	printk("[hw-cloud] reply2=%.60s usage=%u+%u\n", out,
	       usage.prompt_tokens, usage.completion_tokens);
	chat_touched = 0;
	memset(&usage, 0, sizeof(usage));
	ret = chat_once(false, out, sizeof(out), &usage);
	CHK(ret == ENTELEA_OK && out[0] != '\0' &&
		    usage.completion_tokens > 0 && chat_touched >= 1,
	    "ordinary model_ask after recovery");
	CHK(disconnect_quiet() == 0, "disconnect after model call");

	net_mgmt_del_event_callback(&wifi_cb);
	net_mgmt_del_event_callback(&ipv4_cb);
	net_mgmt_del_event_callback(&iface_cb);
	entelea_model_bind(NULL, NULL);
	entelea_secure_zero(&credentials, sizeof(credentials));
	entelea_secure_zero(&cfg_live, sizeof(cfg_live));
	entelea_secure_zero(out, sizeof(out));
	printk("[hw-cloud] PASS: two recovery probes + gated model_ask\n");
	k_sleep(K_FOREVER);
	return 0;

fail:
	if (callbacks_registered && connect_requested) {
		int cleanup_ret = disconnect_quiet();

		if (cleanup_ret != 0) {
			net_dhcpv4_stop(test_iface);
			printk("[hw-cloud] disconnect unconfirmed rc=%d\n",
			       cleanup_ret);
		}
	}
	if (callbacks_registered) {
		net_mgmt_del_event_callback(&wifi_cb);
		net_mgmt_del_event_callback(&ipv4_cb);
		net_mgmt_del_event_callback(&iface_cb);
	}
	(void) entelea_dep_mark_down(ENTELEA_DEP_NET);
	(void) entelea_dep_mark_down(ENTELEA_DEP_MODEL);
	entelea_model_bind(NULL, NULL);
	entelea_secure_zero(&credentials, sizeof(credentials));
	entelea_secure_zero(&cfg_live, sizeof(cfg_live));
	entelea_secure_zero(out, sizeof(out));
	printk("[hw-cloud] diagnostic stopped; network/model dependencies "
	       "DOWN\n");
	k_sleep(K_FOREVER);
	return 0;
}
