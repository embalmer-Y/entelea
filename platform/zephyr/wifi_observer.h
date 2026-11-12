/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_ZEPHYR_WIFI_OBSERVER_H
#define ENTELEA_ZEPHYR_WIFI_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <entelea/netmon.h>

struct entelea_wifi_observer {
	struct k_spinlock lock;
	bool associated;
	bool bound;
};

enum entelea_wifi_observation {
	ENTELEA_WIFI_ASSOCIATED,
	ENTELEA_WIFI_DISCONNECTED,
	ENTELEA_WIFI_DHCP_BOUND,
	ENTELEA_WIFI_DHCP_LOST,
};

static inline void
entelea_wifi_observer_event(struct entelea_wifi_observer *obs,
			    enum entelea_wifi_observation event,
			    uint64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&obs->lock);

	switch (event) {
	case ENTELEA_WIFI_ASSOCIATED:
		obs->associated = true;
		obs->bound = false;
		break;
	case ENTELEA_WIFI_DISCONNECTED:
		obs->associated = false;
		obs->bound = false;
		(void) entelea_dep_report(ENTELEA_DEP_NET, false, now_ms);
		break;
	case ENTELEA_WIFI_DHCP_BOUND:
		obs->bound = obs->associated;
		break;
	case ENTELEA_WIFI_DHCP_LOST:
		obs->bound = false;
		(void) entelea_dep_report(ENTELEA_DEP_NET, false, now_ms);
		break;
	}
	k_spin_unlock(&obs->lock, key);
}

static inline bool
entelea_wifi_observer_sample(struct entelea_wifi_observer *obs, bool iface_up,
			     uint64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&obs->lock);
	bool good = obs->associated && obs->bound && iface_up;

	(void) entelea_dep_report(ENTELEA_DEP_NET, good, now_ms);
	k_spin_unlock(&obs->lock, key);
	return good;
}

#endif /* ENTELEA_ZEPHYR_WIFI_OBSERVER_H */
