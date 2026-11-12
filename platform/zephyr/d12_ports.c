/* SPDX-License-Identifier: Apache-2.0 */
/* D12 闸的 Zephyr 平台端口：互斥（k_mutex，ISR 中拒绝）与墙钟。墙钟在
 * SNTP 接线（板级授权批次）前恒为未同步，闸门对模型调用 fail-closed。 */
#include <zephyr/kernel.h>
#include <entelea/d12.h>
#include "d12_ports.h"

static K_MUTEX_DEFINE(d12_mutex);

static entelea_result_t
d12_lock_take(void *user)
{
	(void) user;
	if (k_is_in_isr()) {
		return ENTELEA_E_STATE;
	}
	return k_mutex_lock(&d12_mutex, K_FOREVER) == 0 ? ENTELEA_OK :
							  ENTELEA_E_STATE;
}

static void
d12_lock_give(void *user)
{
	(void) user;
	(void) k_mutex_unlock(&d12_mutex);
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

static entelea_result_t
d12_wall_read(void *user, uint64_t *out)
{
	(void) user;
	(void) out;
	return ENTELEA_E_STATE;
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
