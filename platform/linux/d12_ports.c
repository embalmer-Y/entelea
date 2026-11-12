/* SPDX-License-Identifier: Apache-2.0 */
/* D12 闸的 Linux 宿主端口：pthread 互斥、CLOCK_REALTIME 墙钟、进程内存持久。
 * 进程内存持久在重启（进程退出）后不保留日计数，仅用于宿主诊断面；产品闸
 * 的日额持久以板级存储为准。 */
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <entelea/d12.h>
#include "d12_ports.h"

static pthread_mutex_t d12_mutex = PTHREAD_MUTEX_INITIALIZER;

static entelea_result_t d12_lock_take(void *user)
{
	(void)user;
	return pthread_mutex_lock(&d12_mutex) == 0 ? ENTELEA_OK
						   : ENTELEA_E_STATE;
}

static void d12_lock_give(void *user)
{
	(void)user;
	(void)pthread_mutex_unlock(&d12_mutex);
}

entelea_result_t entelea_platform_d12_lock_init(entelea_lock_t *lock)
{
	if (lock == NULL) {
		return ENTELEA_E_PARAM;
	}
	lock->lock = d12_lock_take;
	lock->unlock = d12_lock_give;
	lock->user = NULL;
	return ENTELEA_OK;
}

/* 宿主墙钟视为已同步（CLOCK_REALTIME）；读失败即未知。 */
static entelea_result_t d12_wall_read(void *user, uint64_t *out)
{
	struct timespec ts;

	(void)user;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) {
		return ENTELEA_E_STATE;
	}
	*out = (uint64_t)ts.tv_sec;
	return ENTELEA_OK;
}

entelea_result_t entelea_platform_d12_wallclock_init(entelea_wallclock_t *wall)
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

static entelea_result_t d12_store_load(void *user,
				       uint8_t rec[ENTELEA_D12_REC_LEN])
{
	(void)user;
	if (!d12_rec_present) {
		return ENTELEA_E_NOTFOUND;
	}
	memcpy(rec, d12_rec, ENTELEA_D12_REC_LEN);
	return ENTELEA_OK;
}

static entelea_result_t d12_store_save(void *user,
				       const uint8_t rec[ENTELEA_D12_REC_LEN])
{
	(void)user;
	memcpy(d12_rec, rec, ENTELEA_D12_REC_LEN);
	d12_rec_present = true;
	return ENTELEA_OK;
}

entelea_result_t entelea_platform_d12_store_init(entelea_d12_store_t *store)
{
	if (store == NULL) {
		return ENTELEA_E_PARAM;
	}
	store->load = d12_store_load;
	store->save = d12_store_save;
	store->user = NULL;
	return ENTELEA_OK;
}
