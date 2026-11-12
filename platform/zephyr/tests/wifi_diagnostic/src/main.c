/* SPDX-License-Identifier: Apache-2.0 */
/* Execute the real diagnostic main with UART and network calls replaced. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
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
#include <zephyr/ztest.h>
#include <entelea/d12.h>
#include <entelea/events.h>
#include <entelea/model.h>
#include <entelea/netmon.h>
#include <entelea/safety.h>
#include <entelea/supervision.h>
#include <entelea/wdt.h>
#include "boot_steps.h"
#include "clock.h"
#include "https_chat.h"

static struct net_if *fake_default_iface(void);
static const struct device *fake_console_device(void);
static const struct device *fake_iface_device(struct net_if *iface);
static bool fake_device_ready(const struct device *dev);
static bool fake_iface_is_up(struct net_if *iface);
static int fake_iface_up(struct net_if *iface);
static int fake_uart_poll(const struct device *dev, unsigned char *byte);
static int fake_sem_take(struct k_sem *sem, k_timeout_t timeout);
static int32_t fake_sleep(k_timeout_t timeout);
static int fake_net_mgmt(uint64_t request, struct net_if *iface,
			 void *data, size_t length);
static void fake_dhcp_start(struct net_if *iface);
static void fake_dhcp_stop(struct net_if *iface);
static void fake_callback_init(struct net_mgmt_event_callback *cb,
			       net_mgmt_event_handler_t handler, uint64_t mask);
static void fake_callback_add(struct net_mgmt_event_callback *cb);
static void fake_callback_del(struct net_mgmt_event_callback *cb);
static int fake_printk(const char *format, ...);

#undef net_mgmt
#undef DEVICE_DT_GET
#define DEVICE_DT_GET(node_id) fake_console_device()
#define net_mgmt fake_net_mgmt
#define net_if_get_default fake_default_iface
#define net_if_get_device fake_iface_device
#define net_if_is_up fake_iface_is_up
#define net_if_up fake_iface_up
#define device_is_ready fake_device_ready
#define uart_poll_in fake_uart_poll
#define k_sem_take fake_sem_take
#define k_sleep fake_sleep
#define net_dhcpv4_start fake_dhcp_start
#define net_dhcpv4_stop fake_dhcp_stop
#define net_mgmt_init_event_callback fake_callback_init
#define net_mgmt_add_event_callback fake_callback_add
#define net_mgmt_del_event_callback fake_callback_del
#define printk fake_printk
#define main diagnostic_main_under_test
#ifdef TEST_CLOUD_DIAGNOSTIC
#include "../../../apps/hw_cloud_chat/main.c"
#else
#include "../../../apps/hw_wifi_netmon/main.c"
#endif
#undef main
#undef printk
#undef net_mgmt_del_event_callback
#undef net_mgmt_add_event_callback
#undef net_mgmt_init_event_callback
#undef net_dhcpv4_stop
#undef net_dhcpv4_start
#undef k_sleep
#undef k_sem_take
#undef uart_poll_in
#undef device_is_ready
#undef net_if_up
#undef net_if_is_up
#undef net_if_get_device
#undef net_if_get_default
#undef DEVICE_DT_GET
#undef net_mgmt

static struct net_if fake_iface;
static const struct device fake_uart_device;
static uint8_t input[128];
static size_t input_len;
static size_t input_pos;
static const uint8_t *observed_ssid;
static const uint8_t *observed_psk;
static int callbacks_live;
static int connect_calls;
static int disconnect_calls;
static int disconnect_while_registered;
static int connect_result;
static int disconnect_result;
static int disconnect_event_status;
static int dhcp_start_calls;
static int dhcp_stop_calls;
static bool connect_completes;
static bool disconnect_times_out;
static bool unconfirmed_logged;
static bool safe_halt;
static entelea_dep_state_t dep_state[ENTELEA_DEP_COUNT];
static unsigned int dep_good_count[ENTELEA_DEP_COUNT];
#ifdef TEST_CLOUD_DIAGNOSTIC
static bool model_succeeds;
static uint32_t model_calls;
static entelea_model_usage_t model_usage;
#endif

const entelea_boot_step_t entelea_platform_boot_steps[ENTELEA_PLATFORM_BOOT_STEP_COUNT];

entelea_result_t entelea_platform_clock_init(entelea_clock_t *clock)
{
	ARG_UNUSED(clock);
	return ENTELEA_OK;
}

entelea_result_t entelea_evt_bind_clock(const entelea_clock_t *clock)
{
	ARG_UNUSED(clock);
	return ENTELEA_OK;
}

entelea_result_t entelea_wdt_bind_clock(const entelea_clock_t *clock)
{
	ARG_UNUSED(clock);
	return ENTELEA_OK;
}

entelea_result_t entelea_safety_bind_clock(const entelea_clock_t *clock)
{
	ARG_UNUSED(clock);
	return ENTELEA_OK;
}

void entelea_safety_system_fail(uint32_t reason)
{
	ARG_UNUSED(reason);
	zassert_unreachable("ordinary diagnostic failure escalated to system_fail");
}

void entelea_supervision_boot(const entelea_boot_step_t *steps, size_t count)
{
	ARG_UNUSED(steps);
	ARG_UNUSED(count);
}

bool entelea_platform_d12_safe_halt(void)
{
	return safe_halt;
}

entelea_result_t entelea_dep_report(entelea_dep_kind_t kind, bool good, uint64_t now)
{
	ARG_UNUSED(now);
	if (good) {
		if (dep_good_count[kind] < 2) {
			dep_good_count[kind]++;
		}
		dep_state[kind] = dep_good_count[kind] == 2 ?
			ENTELEA_DEP_UP : ENTELEA_DEP_DEGRADED;
	} else {
		dep_good_count[kind] = 0;
		dep_state[kind] = ENTELEA_DEP_DOWN;
	}
	return ENTELEA_OK;
}

entelea_result_t entelea_dep_mark_down(entelea_dep_kind_t kind)
{
	dep_state[kind] = ENTELEA_DEP_DOWN;
	dep_good_count[kind] = 0;
	return ENTELEA_OK;
}

entelea_dep_state_t entelea_dep_state(entelea_dep_kind_t kind)
{
	return dep_state[kind];
}

#ifdef TEST_CLOUD_DIAGNOSTIC
uint32_t entelea_d12_model_calls(void) { return model_calls; }
void entelea_model_bind(const entelea_model_transport_t *ops, void *user)
{
	ARG_UNUSED(ops);
	ARG_UNUSED(user);
}
entelea_model_usage_t entelea_model_usage_total(void)
{
	return model_usage;
}
entelea_result_t entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n,
				   char *out, size_t out_max, uint32_t timeout,
				   entelea_model_usage_t *usage)
{
	ARG_UNUSED(msgs); ARG_UNUSED(n); ARG_UNUSED(timeout);
	if (!model_succeeds || dep_state[ENTELEA_DEP_NET] != ENTELEA_DEP_UP) {
		return ENTELEA_E_STATE;
	}
	if (out_max < 3) {
		return ENTELEA_E_RANGE;
	}
	chat_touched++;
	model_calls++;
	model_usage.prompt_tokens++;
	model_usage.completion_tokens++;
	*usage = model_usage;
	memcpy(out, "ok", 3);
	return ENTELEA_OK;
}
entelea_result_t entelea_model_probe(const entelea_model_msg_t *msgs, uint8_t n,
				     char *out, size_t out_max, uint32_t timeout,
				     entelea_model_usage_t *usage)
{
	return entelea_model_ask(msgs, n, out, out_max, timeout, usage);
}
entelea_result_t entelea_https_chat(void *user, const entelea_model_msg_t *msgs,
				    uint8_t n, char *out, size_t out_max,
				    uint32_t timeout, entelea_model_usage_t *usage)
{
	ARG_UNUSED(user);
	return entelea_model_ask(msgs, n, out, out_max, timeout, usage);
}
#endif

static struct net_if *fake_default_iface(void) { return &fake_iface; }
static const struct device *fake_console_device(void) { return &fake_uart_device; }
static const struct device *fake_iface_device(struct net_if *iface)
{
	ARG_UNUSED(iface);
	return fake_console_device();
}
static bool fake_device_ready(const struct device *dev)
{
	ARG_UNUSED(dev);
	return true;
}
static bool fake_iface_is_up(struct net_if *iface) { ARG_UNUSED(iface); return true; }
static int fake_iface_up(struct net_if *iface) { ARG_UNUSED(iface); return 0; }
static int fake_uart_poll(const struct device *dev, unsigned char *byte)
{
	ARG_UNUSED(dev);
	if (input_pos >= input_len) {
		return -1;
	}
	*byte = input[input_pos++];
	return 0;
}
static int fake_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	if (sem == &connected) {
		if (!connect_completes) {
			return -ETIMEDOUT;
		}
		struct wifi_status status = {.status = 0};

		wifi_cb.info = &status;
		wifi_cb.info_length = sizeof(status);
		wifi_cb.handler(&wifi_cb, NET_EVENT_WIFI_CONNECT_RESULT, test_iface);
		return 0;
	}
	if (sem == &dhcp_bound) {
		return 0;
	}
	if (sem == &disconnected) {
		if (disconnect_times_out) {
			return -ETIMEDOUT;
		}
		struct wifi_status status = {.status = disconnect_event_status};

		wifi_cb.info = &status;
		wifi_cb.info_length = sizeof(status);
		wifi_cb.handler(&wifi_cb, NET_EVENT_WIFI_DISCONNECT_RESULT,
				test_iface);
		return 0;
	}
	return -ETIMEDOUT;
}
static int32_t fake_sleep(k_timeout_t timeout) { ARG_UNUSED(timeout); return 0; }
static int fake_net_mgmt(uint64_t request, struct net_if *iface,
			 void *data, size_t length)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(length);
	if (request == NET_REQUEST_WIFI_CONNECT) {
		const struct wifi_connect_req_params *params = data;

		connect_calls++;
		observed_ssid = params->ssid;
		observed_psk = params->psk;
		return connect_result;
	}
	if (request == NET_REQUEST_WIFI_DISCONNECT) {
		disconnect_calls++;
		if (callbacks_live == 3) {
			disconnect_while_registered++;
		}
		return disconnect_result;
	}
	return -EINVAL;
}
static void fake_dhcp_start(struct net_if *iface)
{
	dhcp_start_calls++;
	ipv4_cb.handler(&ipv4_cb, NET_EVENT_IPV4_DHCP_BOUND, iface);
}
static void fake_dhcp_stop(struct net_if *iface)
{
	ARG_UNUSED(iface);
	dhcp_stop_calls++;
}
static void fake_callback_init(struct net_mgmt_event_callback *cb,
			       net_mgmt_event_handler_t handler, uint64_t mask)
{
	cb->handler = handler;
	cb->event_mask = mask;
}
static void fake_callback_add(struct net_mgmt_event_callback *cb)
{
	ARG_UNUSED(cb);
	callbacks_live++;
}
static void fake_callback_del(struct net_mgmt_event_callback *cb)
{
	ARG_UNUSED(cb);
	callbacks_live--;
}
static int fake_printk(const char *format, ...)
{
	char text[256];
	va_list ap;

	va_start(ap, format);
	int ret = vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	if (strstr(text, "disconnect unconfirmed") != NULL) {
		unconfirmed_logged = true;
	}
	return ret;
}

static void reset_fixture(void *fixture)
{
	ARG_UNUSED(fixture);
	static const uint8_t ssid_psk[] = "abc12345678";
#ifdef TEST_CLOUD_DIAGNOSTIC
	static const uint8_t header[] = {'P', '4', 'C', '2', 3, 8, 1, 20, 1};
#else
	static const uint8_t header[] = {'P', '4', 'C', '1', 3, 8};
#endif
	memcpy(input, header, sizeof(header));
	memcpy(input + sizeof(header), ssid_psk, sizeof(ssid_psk) - 1);
	input_len = sizeof(header) + sizeof(ssid_psk) - 1;
#ifdef TEST_CLOUD_DIAGNOSTIC
	input[input_len++] = 'h';
	memset(input + input_len, 'k', 20);
	input_len += 20;
	input[input_len++] = 'm';
#endif
	input_pos = 0;
	observed_ssid = NULL;
	observed_psk = NULL;
	callbacks_live = 0;
	connect_calls = 0;
	disconnect_calls = 0;
	disconnect_while_registered = 0;
	connect_result = 0;
	disconnect_result = 0;
	disconnect_event_status = 0;
	dhcp_start_calls = 0;
	dhcp_stop_calls = 0;
	connect_completes = false;
	disconnect_times_out = false;
	connect_requested = false;
	unconfirmed_logged = false;
	safe_halt = false;
	memset(dep_state, 0, sizeof(dep_state));
	memset(dep_good_count, 0, sizeof(dep_good_count));
#ifdef TEST_CLOUD_DIAGNOSTIC
	model_succeeds = false;
	model_calls = 0;
	model_usage = (entelea_model_usage_t){0};
#endif
}

static void assert_cleanup(void)
{
	zassert_equal(connect_calls, 1);
	zassert_equal(disconnect_calls, 1);
	zassert_equal(disconnect_while_registered, 1);
	zassert_equal(callbacks_live, 0);
	zassert_equal(dep_state[ENTELEA_DEP_NET], ENTELEA_DEP_DOWN);
	zassert_not_null(observed_ssid);
	zassert_not_null(observed_psk);
	for (size_t i = 0; i <= ENTELEA_WIFI_SSID_MAX; i++) {
		zassert_equal(observed_ssid[i], 0);
	}
	for (size_t i = 0; i <= ENTELEA_WIFI_PSK_MAX; i++) {
		zassert_equal(observed_psk[i], 0);
	}
	zassert_equal(dhcp_stop_calls, 1);
}

ZTEST(wifi_diagnostic, test_timeout_cancels_pending_association)
{
	zassert_equal(diagnostic_main_under_test(), 0);
	assert_cleanup();
	zassert_false(connect_requested);
	zassert_false(unconfirmed_logged);
}

ZTEST(wifi_diagnostic, test_connect_error_with_unknown_effect_is_cancelled)
{
	connect_result = -EIO;
	zassert_equal(diagnostic_main_under_test(), 0);
	assert_cleanup();
}

ZTEST(wifi_diagnostic, test_cancel_failure_is_reported)
{
	disconnect_result = -EIO;
	zassert_equal(diagnostic_main_under_test(), 0);
	assert_cleanup();
	zassert_true(unconfirmed_logged);
	zassert_true(connect_requested);
}

ZTEST(wifi_diagnostic, test_disconnect_timeout_is_reported)
{
	disconnect_times_out = true;
	zassert_equal(diagnostic_main_under_test(), 0);
	assert_cleanup();
	zassert_true(unconfirmed_logged);
	zassert_true(connect_requested);
}

ZTEST(wifi_diagnostic, test_disconnect_event_failure_is_reported)
{
	disconnect_event_status = -EIO;
	zassert_equal(diagnostic_main_under_test(), 0);
	assert_cleanup();
	zassert_true(unconfirmed_logged);
	zassert_true(connect_requested);
}

ZTEST(wifi_diagnostic, test_bad_frame_never_connects)
{
	input[0] = 'X';
	zassert_equal(diagnostic_main_under_test(), 0);
	zassert_equal(connect_calls, 0);
	zassert_equal(disconnect_calls, 0);
	zassert_equal(callbacks_live, 0);
	zassert_equal(dep_state[ENTELEA_DEP_NET], ENTELEA_DEP_DOWN);
}

ZTEST(wifi_diagnostic, test_completed_associations_are_not_cancelled_again)
{
	connect_completes = true;
#ifdef TEST_CLOUD_DIAGNOSTIC
	model_succeeds = true;
#endif
	zassert_equal(diagnostic_main_under_test(), 0);
	zassert_equal(connect_calls, 2);
	zassert_equal(disconnect_calls, 2);
	zassert_equal(disconnect_while_registered, 2);
	zassert_equal(dhcp_start_calls, 2);
	zassert_equal(dhcp_stop_calls, 2);
	zassert_equal(callbacks_live, 0);
	zassert_false(connect_requested);
	zassert_false(unconfirmed_logged);
#ifdef TEST_CLOUD_DIAGNOSTIC
	zassert_equal(model_calls, 3);
#endif
}

#ifdef TEST_CLOUD_DIAGNOSTIC
ZTEST(wifi_diagnostic, test_safe_halt_prevents_credentials_and_network)
{
	safe_halt = true;
	zassert_equal(diagnostic_main_under_test(), 0);
	zassert_equal(input_pos, 0);
	zassert_equal(connect_calls, 0);
	zassert_equal(disconnect_calls, 0);
}
#endif

ZTEST_SUITE(wifi_diagnostic, NULL, NULL, reset_fixture, NULL, NULL);
