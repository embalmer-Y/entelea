/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise concurrent dependency observations on the actual framework source. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <entelea/netmon.h>
#include "wifi_observer.h"

static pthread_barrier_t barrier;
static int failed;
static struct entelea_wifi_observer observer;

static int
check_observer(void)
{
	entelea_wifi_observer_event(&observer, ENTELEA_WIFI_ASSOCIATED, 1);
	entelea_wifi_observer_event(&observer, ENTELEA_WIFI_DHCP_BOUND, 2);
	if (!entelea_wifi_observer_sample(&observer, true, 3) ||
	    entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DEGRADED) {
		return 1;
	}
	entelea_wifi_observer_event(&observer, ENTELEA_WIFI_DISCONNECTED, 4);
	if (entelea_wifi_observer_sample(&observer, true, 5) ||
	    entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DOWN) {
		return 1;
	}
	entelea_wifi_observer_event(&observer, ENTELEA_WIFI_ASSOCIATED, 6);
	entelea_wifi_observer_event(&observer, ENTELEA_WIFI_DHCP_BOUND, 7);
	if (!entelea_wifi_observer_sample(&observer, true, 8) ||
	    !entelea_wifi_observer_sample(&observer, true, 9) ||
	    entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP) {
		return 1;
	}
	entelea_wifi_observer_event(&observer, ENTELEA_WIFI_DHCP_LOST, 10);
	return entelea_wifi_observer_sample(&observer, true, 11) ||
	       entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DOWN;
}

static void *
good_worker(void *arg)
{
	(void) arg;
	(void) pthread_barrier_wait(&barrier);
	for (uint64_t i = 0; i < 100000; i++) {
		if (entelea_dep_report(ENTELEA_DEP_NET, true, i) !=
		    ENTELEA_OK) {
			return &failed;
		}
	}
	return NULL;
}

static void *
bad_worker(void *arg)
{
	(void) arg;
	(void) pthread_barrier_wait(&barrier);
	for (uint64_t i = 0; i < 100000; i++) {
		if (entelea_dep_report(ENTELEA_DEP_NET, false, i) !=
		    ENTELEA_OK) {
			return &failed;
		}
		(void) entelea_dep_state(ENTELEA_DEP_NET);
	}
	return NULL;
}

int
main(void)
{
	pthread_t good, bad;
	void *good_result, *bad_result;

	if (pthread_barrier_init(&barrier, NULL, 2) != 0 ||
	    pthread_create(&good, NULL, good_worker, NULL) != 0 ||
	    pthread_create(&bad, NULL, bad_worker, NULL) != 0 ||
	    pthread_join(good, &good_result) != 0 ||
	    pthread_join(bad, &bad_result) != 0 ||
	    pthread_barrier_destroy(&barrier) != 0 || good_result != NULL ||
	    bad_result != NULL ||
	    entelea_dep_report(ENTELEA_DEP_NET, false, 100000) != ENTELEA_OK ||
	    entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_DOWN ||
	    check_observer() != 0) {
		fprintf(stderr, "FAIL: concurrent dependency observations\n");
		return 1;
	}
	puts("PASS: concurrent dependency observations");
	return 0;
}
