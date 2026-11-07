// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <ts/core.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/ztest.h>

#include "diagnostic.h"

enum scan_scenario {
	SCAN_NORMAL,
	SCAN_EMPTY,
	SCAN_EVENT_ERROR,
	SCAN_NO_EVENT,
	SCAN_REQUEST_ERROR,
	SCAN_LATE_EVENT,
};

static enum scan_scenario scenario;
static int requests;
static bool watchdog_fails;
static scan_result_cb_t delayed_cb;
static struct net_if *delayed_iface;
static K_SEM_DEFINE(late_delivered, 0, 1);

ts_res_t __wrap_ts_wdt_start(void)
{
	return watchdog_fails ? TS_E_IO : TS_OK;
}

static void late_scan_done(struct k_work *work)
{
	ARG_UNUSED(work);
	delayed_cb(delayed_iface, 0, NULL);
	k_sem_give(&late_delivered);
}

static K_WORK_DELAYABLE_DEFINE(late_work, late_scan_done);

static int scan_request(const struct device *dev, struct net_if *iface,
			struct wifi_scan_params *params, scan_result_cb_t cb)
{
	struct wifi_scan_result result = {
		.channel = 1,
		.rssi = -30,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	ARG_UNUSED(dev);
	ARG_UNUSED(params);
	requests++;
	switch (scenario) {
	case SCAN_NORMAL:
		cb(iface, 0, &result);
		cb(iface, 0, NULL);
		return 0;
	case SCAN_EMPTY:
		cb(iface, 0, NULL);
		return 0;
	case SCAN_EVENT_ERROR:
		cb(iface, -EIO, NULL);
		return 0;
	case SCAN_NO_EVENT:
		return 0;
	case SCAN_LATE_EVENT:
		delayed_cb = cb;
		delayed_iface = iface;
		k_work_reschedule(&late_work, K_MSEC(10));
		return -ETIMEDOUT;
	case SCAN_REQUEST_ERROR:
		return -ETIMEDOUT;
	}
	zassert_unreachable("Invalid test scenario");
	return -EINVAL;
}

static void scan_iface_init(struct net_if *iface)
{
	static uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};

	net_if_set_link_addr(iface, mac, sizeof(mac), NET_LINK_ETHERNET);
	ethernet_init(iface);
	net_eth_set_if_type_wifi(iface);
}

static const struct wifi_mgmt_ops scan_ops = {
	.scan = scan_request,
};

static const struct net_wifi_mgmt_offload scan_api = {
	.wifi_iface.iface_api.init = scan_iface_init,
	.wifi_mgmt_api = &scan_ops,
};

NET_DEVICE_INIT(scan_device, "scan-test", NULL, NULL, NULL, NULL, CONFIG_WIFI_INIT_PRIORITY,
		&scan_api, ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2), NET_ETH_MTU);

static void scan_before(void *fixture)
{
	struct k_work_sync sync;

	ARG_UNUSED(fixture);
	k_work_cancel_delayable_sync(&late_work, &sync);
	k_sem_reset(&late_delivered);
	scenario = SCAN_NORMAL;
	requests = 0;
	watchdog_fails = false;
	net_if_up(net_if_get_default());
}

ZTEST(scan_diagnostic, test_two_scans_and_callback_reuse)
{
	zassert_equal(c5_scan_diagnostic(), 0);
	zassert_equal(requests, 2);
	zassert_equal(c5_scan_diagnostic(), 0);
	zassert_equal(requests, 4);
}

ZTEST(scan_diagnostic, test_watchdog_failure_stops_before_network)
{
	watchdog_fails = true;
	zassert_equal(c5_scan_diagnostic(), 1);
	zassert_equal(requests, 0);
}

ZTEST(scan_diagnostic, test_request_failure_stops_retry)
{
	scenario = SCAN_REQUEST_ERROR;
	zassert_equal(c5_scan_diagnostic(), 1);
	zassert_equal(requests, 1);
}

ZTEST(scan_diagnostic, test_missing_completion_is_bounded)
{
	uint64_t begin = ts_time_ms();

	scenario = SCAN_NO_EVENT;
	zassert_equal(c5_scan_diagnostic(), 1);
	zassert_equal(requests, 1);
	zassert_true(ts_time_ms() - begin >= 15000);
	zassert_true(ts_time_ms() - begin < 15100);
}

ZTEST(scan_diagnostic, test_failed_completion_is_not_success)
{
	scenario = SCAN_EVENT_ERROR;
	zassert_equal(c5_scan_diagnostic(), 1);
	zassert_equal(requests, 1);
}

ZTEST(scan_diagnostic, test_empty_environment_fails_this_acceptance)
{
	scenario = SCAN_EMPTY;
	zassert_equal(c5_scan_diagnostic(), 1);
	zassert_equal(requests, 1);
}

ZTEST(scan_diagnostic, test_late_completion_cannot_start_another_scan)
{
	scenario = SCAN_LATE_EVENT;
	zassert_equal(c5_scan_diagnostic(), 1);
	zassert_equal(requests, 1);
	zassert_ok(k_sem_take(&late_delivered, K_MSEC(100)));
	zassert_equal(requests, 1);
	scenario = SCAN_NORMAL;
	zassert_equal(c5_scan_diagnostic(), 0);
	zassert_equal(requests, 3);
}

ZTEST_SUITE(scan_diagnostic, NULL, NULL, scan_before, NULL, NULL);
