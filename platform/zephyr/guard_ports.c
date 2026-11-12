/* SPDX-License-Identifier: Apache-2.0 */
/* Zephyr platform ports for the D12 gate: mutex (k_mutex, rejected in ISR
 * context) and wall clock. The wall clock stays unsynchronized until SNTP
 * wiring (board-level authorized batch); the gate fails closed for model
 * calls. */
#include <zephyr/kernel.h>
#include <entelea/guard.h>
#include "guard_ports.h"

static K_MUTEX_DEFINE(guard_mutex);

static entelea_result_t
guard_lock_take(void *user)
{
	(void) user;
	if (k_is_in_isr()) {
		return ENTELEA_E_STATE;
	}
	return k_mutex_lock(&guard_mutex, K_FOREVER) == 0 ? ENTELEA_OK :
							    ENTELEA_E_STATE;
}

static void
guard_lock_give(void *user)
{
	(void) user;
	(void) k_mutex_unlock(&guard_mutex);
}

entelea_result_t
entelea_platform_guard_lock_init(entelea_lock_t *lock)
{
	if (lock == NULL) {
		return ENTELEA_E_PARAM;
	}
	lock->lock = guard_lock_take;
	lock->unlock = guard_lock_give;
	lock->user = NULL;
	return ENTELEA_OK;
}

static entelea_result_t
guard_wall_read(void *user, uint64_t *out)
{
	(void) user;
	(void) out;
	return ENTELEA_E_STATE;
}

entelea_result_t
entelea_platform_guard_wallclock_init(entelea_wallclock_t *wall)
{
	if (wall == NULL) {
		return ENTELEA_E_PARAM;
	}
	wall->read_utc_s = guard_wall_read;
	wall->user = NULL;
	return ENTELEA_OK;
}
