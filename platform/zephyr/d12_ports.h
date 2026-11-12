/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_D12_PORTS_H
#define ENTELEA_PLATFORM_D12_PORTS_H

#include <entelea/d12.h>

/* 填充 D12 平台端口结构，由装配层经 entelea_d12_bind_* 绑定。持久端口
 * 仅在链接 d12_store.c 的目标中有强实现，其余目标经弱缺省返回 E_UNINIT。 */
entelea_result_t entelea_platform_d12_lock_init(entelea_lock_t *lock);
entelea_result_t entelea_platform_d12_wallclock_init(entelea_wallclock_t *wall);
entelea_result_t entelea_platform_d12_store_init(entelea_d12_store_t *store);

#endif /* ENTELEA_PLATFORM_D12_PORTS_H */
