/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LINUX_D12_PORTS_H
#define ENTELEA_LINUX_D12_PORTS_H

#include <entelea/d12.h>

/* 填充 D12 宿主端口结构，由装配层经 entelea_d12_bind_* 绑定。 */
entelea_result_t
entelea_platform_d12_lock_init(entelea_lock_t *lock);
entelea_result_t
entelea_platform_d12_wallclock_init(entelea_wallclock_t *wall);
entelea_result_t
entelea_platform_d12_store_init(entelea_d12_store_t *store);

#endif /* ENTELEA_LINUX_D12_PORTS_H */
