/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_D12_PORTS_H
#define ENTELEA_PLATFORM_D12_PORTS_H

#include <entelea/d12.h>

/* Fill the D12 platform port structures; the assembly layer binds them via
 * entelea_d12_bind_*. The persistent port has a strong implementation only
 * in targets linking d12_store.c; other targets return E_UNINIT through the
 * weak default. */
entelea_result_t
entelea_platform_d12_lock_init(entelea_lock_t *lock);
entelea_result_t
entelea_platform_d12_wallclock_init(entelea_wallclock_t *wall);
entelea_result_t
entelea_platform_d12_store_init(entelea_d12_store_t *store);

#endif /* ENTELEA_PLATFORM_D12_PORTS_H */
