/* SPDX-License-Identifier: Apache-2.0 */
/* Linux host ports for the D12 gate: pthread mutex, CLOCK_REALTIME wall
 * clock, and in-process memory persistence. In-process memory does not keep
 * the daily count across restarts (process exit); it serves only the host
 * diagnostic surface. For the product gate, daily-quota persistence relies on
 * board storage. */
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <entelea/d12.h>
#include "d12_ports.h"

static pthread_mutex_t d12_mutex = PTHREAD_MUTEX_INITIALIZER;

static entelea_result_t
d12_lock_take(void *user)
{
	(void) user;
	return pthread_mutex_lock(&d12_mutex) == 0 ? ENTELEA_OK :
						     ENTELEA_E_STATE;
}

static void
d12_lock_give(void *user)
{
	(void) user;
	(void) pthread_mutex_unlock(&d12_mutex);
}

entelea_result_t
entelea_platform_d12_lock_init(entelea_lock_t *lock)
{
	if (lock == NULL) {
		return ENTELEA_E_PARAM;
	}
	lock->lock = d12_lock_take;
	lock->unlock = d12_lock_give;
	lock->user = NULL;
	return ENTELEA_OK;
}

/* The host wall clock is treated as synchronized (CLOCK_REALTIME); a failed
 * read means the time is unknown. */
static entelea_result_t
d12_wall_read(void *user, uint64_t *out)
{
	struct timespec ts;

	(void) user;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) {
		return ENTELEA_E_STATE;
	}
	*out = (uint64_t) ts.tv_sec;
	return ENTELEA_OK;
}

entelea_result_t
entelea_platform_d12_wallclock_init(entelea_wallclock_t *wall)
{
	if (wall == NULL) {
		return ENTELEA_E_PARAM;
	}
	wall->read_utc_s = d12_wall_read;
	wall->user = NULL;
	return ENTELEA_OK;
}

static uint8_t d12_rec[ENTELEA_D12_REC_LEN];
static bool d12_rec_present;

static entelea_result_t
d12_store_load(void *user, uint8_t rec[ENTELEA_D12_REC_LEN])
{
	(void) user;
	if (!d12_rec_present) {
		return ENTELEA_E_NOTFOUND;
	}
	memcpy(rec, d12_rec, ENTELEA_D12_REC_LEN);
	return ENTELEA_OK;
}

static entelea_result_t
d12_store_save(void *user, const uint8_t rec[ENTELEA_D12_REC_LEN])
{
	(void) user;
	memcpy(d12_rec, rec, ENTELEA_D12_REC_LEN);
	d12_rec_present = true;
	return ENTELEA_OK;
}

entelea_result_t
entelea_platform_d12_store_init(entelea_d12_store_t *store)
{
	if (store == NULL) {
		return ENTELEA_E_PARAM;
	}
	store->load = d12_store_load;
	store->save = d12_store_save;
	store->user = NULL;
	return ENTELEA_OK;
}
