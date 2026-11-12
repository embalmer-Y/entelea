/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PLATFORM_FLASH_STORE_H
#define ENTELEA_PLATFORM_FLASH_STORE_H

/* Board-level flash backend mount (R03 board batch). boot_steps.c holds the
 * weak stub (default E_UNINIT = no flash surface); targets linking
 * flash_store.c get the strong implementation and mount it through the
 * storage_init step. Idempotent. */
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

entelea_result_t
entelea_platform_storage_mount_flash(void);

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PLATFORM_FLASH_STORE_H */
