/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LINUX_D12_PORTS_H
#define ENTELEA_LINUX_D12_PORTS_H

#include <entelea/d12.h>

/* Populate the D12 host port structures; the assembly layer binds them
 * through entelea_d12_bind_*. */
entelea_result_t
entelea_platform_d12_lock_init(entelea_lock_t *lock);
entelea_result_t
entelea_platform_d12_wallclock_init(entelea_wallclock_t *wall);
entelea_result_t
entelea_platform_d12_store_init(entelea_d12_store_t *store);

#endif /* ENTELEA_LINUX_D12_PORTS_H */
