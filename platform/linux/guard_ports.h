/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LINUX_GUARD_PORTS_H
#define ENTELEA_LINUX_GUARD_PORTS_H

#include <entelea/guard.h>

/* Populate the D12 host port structures; the assembly layer binds them
 * through entelea_guard_bind_*. */
entelea_result_t
entelea_platform_guard_lock_init(entelea_lock_t *lock);
entelea_result_t
entelea_platform_guard_wallclock_init(entelea_wallclock_t *wall);
entelea_result_t
entelea_platform_guard_store_init(entelea_guard_store_t *store);

#endif /* ENTELEA_LINUX_GUARD_PORTS_H */
