/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_MEMORY_H
#define ENTELEA_MEMORY_H

/* R10 experience memory contract: see framework/services/memory/contract.md.
 * A bounded chain with write consent, source, and delete tombstones. Nothing
 * is stored without consent. After a delete, derived indexes must not revive
 * the content. Recovery consistency: the chain can be recovered after power
 * loss (the RAM backend is in-process). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/context.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_MEM_ENTRY_MAX 128 /* max payload per entry (suggested) */
#define ENTELEA_MEM_CHAIN_MAX 64 /* bounded; full evicts oldest (counted) */
#define ENTELEA_MEM_KIND_MAX 12

typedef struct {
	uint64_t t_ms;
	uint8_t source_kind; /* entelea_source_kind_t */
	uint8_t kind; /* memory class (suggested enum; consumers interpret) */
	uint16_t len;
	uint8_t data[ENTELEA_MEM_ENTRY_MAX];
} entelea_mem_entry_t;

/** [thread] Writes an entry. consent=false returns E_PERM (capture is not
 *  memory). When full, the oldest entry is evicted and the evicted count is
 *  incremented. Internal write-sequence exhaustion returns E_RANGE. The caller
 *  serializes operations. */
entelea_result_t
entelea_mem_write(bool consent, const entelea_mem_entry_t *e);

/** [thread] Reads the most recent n entries (oldest to newest); returns the
 *  actual number of entries. */
size_t
entelea_mem_read(entelea_mem_entry_t *out, size_t max);

/** [thread] Deletes (tombstone): hides entries in the given range that were
 * written before this call. Later writes are not affected by old
 * tombstones. */
entelea_result_t
entelea_mem_delete(uint64_t t_from_ms, uint64_t t_to_ms);

/** [any] Chain health (entry count and evicted count). */
typedef struct {
	uint16_t count;
	uint32_t evicted;
	uint32_t tombstoned;
} entelea_mem_health_t;

void
entelea_mem_health(entelea_mem_health_t *out);

#ifdef ENTELEA_TEST
void
entelea_mem_test_reset(void);
entelea_result_t
entelea_mem_test_set_write_seq(uint64_t seq);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_MEMORY_H */
