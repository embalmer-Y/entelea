// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <ts/core.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>

static struct net_mgmt_event_callback scan_cb;
static K_SEM_DEFINE(scan_done, 0, 1);
static atomic_t scan_results;
static atomic_t scan_status;

static void scan_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (event == NET_EVENT_WIFI_SCAN_RESULT) {
		atomic_inc(&scan_results);
	} else if (event == NET_EVENT_WIFI_SCAN_DONE) {
		const struct wifi_status *status =
			cb->info_length == sizeof(struct wifi_status) ? cb->info : NULL;

		atomic_set(&scan_status, status ? status->status : -EIO);
		printk("P4N EVENT_DONE t=%llu status=%d\n", (unsigned long long)ts_time_ms(),
		       (int)atomic_get(&scan_status));
		k_sem_give(&scan_done);
	}
}

int main(void)
{
	struct net_if *iface = net_if_get_default();
	int passed = 0;

	printk("P4N C5 scan diagnostic\n");
	if (!iface || !device_is_ready(net_if_get_device(iface))) {
		printk("P4N FAIL network device not ready\n");
		return 1;
	}
	if (ts_wdt_start() != TS_OK) {
		printk("P4N FAIL watchdog start\n");
		return 1;
	}
	printk("P4N DEVICE_READY admin_up=%d\n", net_if_is_admin_up(iface));
	net_mgmt_init_event_callback(&scan_cb, scan_event,
				     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&scan_cb);
	for (int attempt = 1; attempt <= 2; attempt++) {
		struct wifi_scan_params params = {0};
		int ret;

		k_sem_reset(&scan_done);
		atomic_set(&scan_results, 0);
		atomic_set(&scan_status, -EINPROGRESS);
		printk("P4N SCAN_BEGIN attempt=%d t=%llu\n", attempt,
		       (unsigned long long)ts_time_ms());
		ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
		printk("P4N SCAN_RETURN attempt=%d rc=%d t=%llu\n", attempt, ret,
		       (unsigned long long)ts_time_ms());
		if (ret) {
			break;
		}
		ret = k_sem_take(&scan_done, K_SECONDS(15));
		if (ret) {
			printk("P4N SCAN_DONE timeout=%d\n", ret);
			break;
		}
		int status = atomic_get(&scan_status);
		int count = atomic_get(&scan_results);

		printk("P4N SCAN_DONE status=%d results=%d\n", status, count);
		if (status || count <= 0) {
			break;
		}
		passed++;
		k_sleep(K_SECONDS(1));
	}
	net_mgmt_del_event_callback(&scan_cb);
	printk("P4N %s scans=%d/2\n", passed == 2 ? "PASS" : "FAIL", passed);
	return passed == 2 ? 0 : 1;
}
