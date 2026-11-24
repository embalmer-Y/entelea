/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_STORAGE_H
#define ENTELEA_STORAGE_H

/* R03 storage port contract: see framework/services/storage/contract.md.
 * Each partition is a separate namespace; cross-partition out-of-bounds access
 * is refused. The backend is opaque to callers. Multi-byte integers in
 * internal partitions are little-endian (unrelated to the big-endian TSAP
 * container). Trimming relative to the legacy ts-store, and the deferred
 * subset, are recorded in the contract and the ledger. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Partition namespaces. IDs are append-only and existing values never change;
 * surfaces not yet implemented are added per sub-loop, batch by batch. */
typedef enum {
	ENTELEA_PART_META, /* power-loss-safe small state (two copies) */
	ENTELEA_PART_NOINIT, /* reset trace (one-shot) */
	ENTELEA_PART_AUDIT, /* audit two-page ping-pong ring (sub-loop 2) */
	ENTELEA_PART_PROV, /* provisioning (sub-loop 3, read-only load) */
	ENTELEA_PART_TRUST, /* trust-root table, two copies (sub-loop 4) */
	ENTELEA_PART_KV, /* App business state, two-page append (sub-loop 4) */
	ENTELEA_PART_SLOT_A, /* APP package slot A (R05) */
	ENTELEA_PART_SLOT_B, /* APP package slot B (R05) */
	ENTELEA_PART_COUNT,
} entelea_store_part_t;
/* ---- Backend port: flash backend mounts here; upper layers unaware ---- */

typedef struct {
	entelea_result_t (*read)(entelea_store_part_t part, uint32_t off,
				 void *buf, uint32_t len);
	entelea_result_t (*write)(entelea_store_part_t part, uint32_t off,
				  const void *buf, uint32_t len);
	entelea_result_t (*erase)(entelea_store_part_t part);
	entelea_result_t (*erase_off)(entelea_store_part_t part, uint32_t off,
				      uint32_t len);
	uint32_t (*size)(entelea_store_part_t part);
} entelea_store_ops_t;

#define ENTELEA_STORAGE_META_REC \
	512U /* meta two-copy stride (each copy is a complete record; the flash
	 * backend uses this to place the copies in separate erase sectors). */

/* Current backend (default: in-process RAM backend). It is not const, so the
 * mount point can be replaced. */
extern entelea_store_ops_t entelea_store_backend;

/** [boot] Mounts a backend. All ops functions must be non-NULL; NULL restores
 * the default RAM backend. Mount before storage_init (within a boot step).
 * After a backend change, each partition's logical size is unchanged: the RAM
 * template sets the size and is authoritative. Physical layout (erase-block
 * alignment and padding) belongs to the backend. Returns E_PARAM if ops is
 * incomplete. */
entelea_result_t
entelea_storage_mount(const entelea_store_ops_t *ops);

/** [boot] Storage init: backend ready, noinit consumption, and audit cursor.
 * Failures are returned explicitly; upper layers handle them fail-safe. */
entelea_result_t
entelea_storage_init(void);

/* ---- Shared storage port (cross-module; out-of-namespace refused) ---- */

entelea_result_t
entelea_storage_read(entelea_store_part_t part, uint32_t off, void *buf,
		     uint32_t len);
entelea_result_t
entelea_storage_write(entelea_store_part_t part, uint32_t off, const void *buf,
		      uint32_t len);
entelea_result_t
entelea_storage_erase(entelea_store_part_t part);
uint32_t
entelea_storage_part_size(entelea_store_part_t part);

/** [boot] Trace of the last abnormal reset (queryable after init consumes it).
 * present=false means none or corrupted. */
void
entelea_storage_last_reset(uint32_t *reason, bool *present);

/* ---- Power-loss-safe meta: two copies, sequence, CRC16-CCITT ---- */

/** [thread] Writes the inactive copy, reads it back to verify, and makes it
 * active (seq increases monotonically; if the write is torn, the old copy
 * stays intact). len <= ENTELEA_STORAGE_META_MAX. */
entelea_result_t
entelea_storage_meta_write(const void *buf, uint16_t len);

/** [thread] The active copy is the one with the larger sequence that passes
 * CRC. A single damaged copy is recoverable. If both copies are damaged,
 * returns ENTELEA_E_IO (the caller falls back to a safe default or fail-safe;
 * nothing is fabricated). cap is the buffer capacity: if the active record is
 * longer than cap, returns ENTELEA_E_RANGE and neither buf nor len is written
 * (RF1: the record was previously written out at its full length, overrunning
 * the caller's buffer). */
entelea_result_t
entelea_storage_meta_read(void *buf, uint16_t cap, uint16_t *len);

/** [thread] Both copies must be readable, have valid CRCs, and have identical
 * content; otherwise E_IO. For callers that cannot accept a fallback to an
 * older record. Insufficient cap returns E_RANGE. */
entelea_result_t
entelea_storage_meta_read_mirrored(void *buf, uint16_t cap, uint16_t *len);

/* ---- noinit trace (one-shot) ---- */

/** [any] Writes a trace, overwriting any previous one. A failure cannot be
 * returned: it is silently dropped, and the read surface reports no trace. */
void
entelea_storage_noinit_put(const void *rec, uint16_t len);

/** [thread] Reads the trace. fresh=true means none or corrupted (treated as no
 * trace). A valid record read sets fresh=false and **erases it after reading**
 * (one-shot: prevents a stale trace from being misreported across resets). */
entelea_result_t
entelea_storage_noinit_get(void *rec, uint16_t *len, bool *fresh);

/* ---- Audit ping-pong ring (power-loss retained; sub-loop 2) ---- */

typedef struct {
	uint64_t t_ms;
	uint32_t seq;
	uint16_t ch_idx; /* 0xFFFF = system event (e.g. estop) */
	uint16_t actor;
	uint8_t kind;
	int32_t res; /* entelea_result_t (negative error code) */
	uint16_t evt; /* 0 = commit audit; 1 = estop edge-triggered */
	uint32_t value_u;
} entelea_storage_audit_entry_t;

/** [boot] Scans both pages to recover the cursor. Half-written, bad and
 * foreign slots are invisible and do not truncate the tail. A read error
 * returns E_IO and leaves the ring not ready (a failed read is not an empty
 * ring); so does a committed record of this ring that fails verification.
 * Appends before ready are refused (E_STATE). */
entelea_result_t
entelea_storage_audit_init(void);

/** [thread] Slots of foreign content the last audit_init scan found: content
 * that carries none of this ring's markers (for example another firmware's
 * data in the partition). The ring treats them as absent; they are reported
 * so that the boot log can say so. 0 = none. */
unsigned
entelea_storage_audit_foreign_slots(void);

/** [thread] Appends one entry. The body, CRC, magic, and independent commit
 * marker are written in order and read back. When the 64 slots are full, the
 * page flips. A write failure returns E_IO; the effect may already be on
 * flash, and the slot is not rewritten. For low-frequency abnormal events. */
entelea_result_t
entelea_storage_audit_append(const entelea_storage_audit_entry_t *e,
			     uint16_t evt);

/** [thread] Reads the most recent max entries in ascending seq order (up to
 * 128 during the two-page transition). Returns the count, or a negative error
 * code: E_PARAM if out is NULL, E_STATE if not ready, E_IO on a read error or a
 * corrupted protected record. */
int32_t
entelea_storage_audit_read(entelea_storage_audit_entry_t *out, size_t max);

#include <entelea/kv.h>
#include <entelea/trust.h>

/* ---- SHA-256 streaming (package verification; same as R01) ---- */
void
entelea_sha256_stream_init(uint32_t state[8]);
void
entelea_sha256_stream_update(uint32_t state[8], const uint8_t *data,
			     uint32_t len);
void
entelea_sha256_stream_final(const uint32_t state[8], uint8_t out[32]);

/* Size source: DEC-23/27 semantics (baseline structural values; the board
 * adjusts them via DT). */
/* Meta record limit: copy stride 512 minus an 8-byte header. The trial and
 * stable sets share one record (v4 is 409B). */
#define ENTELEA_STORAGE_META_MAX 448
#define ENTELEA_STORAGE_NOINIT_MAX_REC 250

/* ENTELEA_TEST builds only: erase all partitions (test isolation) and
 * inject torn writes. */
#ifdef ENTELEA_TEST
entelea_result_t
entelea_storage_test_reset(void);
entelea_result_t
entelea_storage_meta_corrupt_test(uint8_t copy_idx);
/* Port-boundary negative tests and torn-write injection probes: these pass
 * straight through to the backend read and write. */
entelea_result_t
entelea_storage_test_backend_read(entelea_store_part_t part, uint32_t off,
				  void *buf, uint32_t len);
entelea_result_t
entelea_storage_test_backend_write(entelea_store_part_t part, uint32_t off,
				   const void *buf, uint32_t len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_STORAGE_H */
