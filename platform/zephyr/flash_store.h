/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_FLASH_STORE_H
#define ENTELEA_PLATFORM_FLASH_STORE_H

/* 板级 flash 后端挂载（R03 板级批）。boot_steps.c 持弱桩（默认
 * E_UNINIT=无 flash 面）；链接 flash_store.c 的目标获得强实现并经
 * storage_init 步骤挂载。幂等。 */
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

entelea_result_t entelea_platform_storage_mount_flash(void);

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PLATFORM_FLASH_STORE_H */
