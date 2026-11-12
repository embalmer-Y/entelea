/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_GUARD_PORTS_H
#define ENTELEA_PLATFORM_GUARD_PORTS_H

#include <entelea/guard.h>

/* Fill the D12 platform port structures; the assembly layer binds them via
 * entelea_guard_bind_*. The persistent port has a strong implementation only
 * in targets linking guard_store.c; other targets return E_UNINIT through the
 * weak default. */
entelea_result_t
entelea_platform_guard_lock_init(entelea_lock_t *lock);
entelea_result_t
entelea_platform_guard_wallclock_init(entelea_wallclock_t *wall);
entelea_result_t
entelea_platform_guard_store_init(entelea_guard_store_t *store);

#endif /* ENTELEA_PLATFORM_GUARD_PORTS_H */
