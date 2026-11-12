/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_KV_H
#define ENTELEA_KV_H

/* R03 App business-state kv contract: see
 * framework/services/storage/contract.md.
 * Bounded blobs (<= 250B) are owned by app_id hash and survive upgrade and
 * rollback. This is not an experience store. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_KV_BLOB_MAX 250 /* 264B slot - 14B header (structural) */

/** Lazy initialization (the first save/load may also call it internally).
 *  A missing partition gives E_STATE; a read error gives E_IO and leaves the
 *  module not ready (the next call rescans and never writes blindly). */
entelea_result_t
entelea_kv_init(void);

/** Saves (overwrites) the state blob of this app. The write is append-only;
 *  the CRC and commit marker are read back afterwards. When the page is full,
 *  live data is read first, then the page is erased and compacted. If the
 *  target page holds records to keep that the active page lacks, returns
 *  E_STATE without erasing. Failure returns an error code. After a write
 *  failure the effect may already be on flash, so the slot is not reused;
 *  rescan before deciding again. */
entelea_result_t
entelea_kv_save(uint32_t app_hash, const uint8_t *data, uint16_t len);

/** Reads the latest blob of this app. Returns the copied length (>= 0) or an
 *  error code: E_PARAM if out is NULL, E_NOTFOUND if nothing is stored,
 *  E_RANGE if the capacity is insufficient, E_IO on a read error or a corrupted
 *  protected record. Init errors are returned unchanged. Legacy records without
 *  a check can still be read, but their integrity cannot be proven. */
int32_t
entelea_kv_load(uint32_t app_hash, uint8_t *out, uint16_t max);

#ifdef ENTELEA_TEST
/** Resets the cursor; the backend is rescanned after erase (test isolation). */
void
entelea_kv_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_KV_H */
