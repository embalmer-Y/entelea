/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_TRUST_H
#define ENTELEA_TRUST_H

/* R03 trust-root table contract: see framework/services/storage/contract.md
 * (section: trust table).
 * A TUF-root-style full-replacement table. Revocation = drop the key in a
 * minor version. There is no separate revoked list: full replacement removes
 * the "fixed-size revocation table overflow revives old keys" issue. Layer
 * responsibilities: this layer only handles persistence and deterministic
 * schema walk-through; signature and monotonic ver gating happen in the R12
 * orchestration chain above it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_TRUST_KEYS_MAX 8
#define ENTELEA_TRUST_CBOR_MAX 512 /* schema check limit (Q-43, structural) */

typedef struct {
	uint32_t ver; /* monotonic version (>= 1; 0 = sentinel) */
	uint8_t n_keys; /* 0..MAX (0 = all revoked; factory root only) */
	uint8_t keys[ENTELEA_TRUST_KEYS_MAX][32];
} entelea_trust_table_t;

/** [boot/first access] Lazily loads the table. The active copy is the one with
 * the larger seq that passes the CRC and schema checks. If both copies are
 * damaged or empty, the result is {0,0}, i.e. factory root only (fail-safe
 * side). Idempotent. */
entelea_result_t
entelea_trust_load(void);

/** [any] Read-only pointer (lazily loads if not yet loaded). */
const entelea_trust_table_t *
entelea_trust(void);

/** Deterministic schema walk-through. The CBOR must be a map
 * {"ver":uint>=1, "keys":[bstr(32)x0..8]} with exactly two pairs in canonical
 * key order. Exceeding a limit, duplicate keys, out-of-domain values, or
 * trailing bytes return E_IO. */
entelea_result_t
entelea_trust_validate(const uint8_t *cbor, uint32_t len,
		       entelea_trust_table_t *out);

/** [thread] Validates, then writes ping-pong style: writes the inactive copy,
 * reads it back and verifies it, switches the active copy, then reloads. An
 * invalid table writes nothing (fail-closed). The caller is the R12
 * orchestration chain, which performs signature and ver gating above this
 * layer. */
entelea_result_t
entelea_trust_write(const uint8_t *cbor, uint32_t len);

#ifdef ENTELEA_TEST
void
entelea_trust_test_reset(void);
entelea_result_t
entelea_trust_corrupt_test(uint8_t copy_idx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_TRUST_H */
