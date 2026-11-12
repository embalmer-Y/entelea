/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_CAPABILITY_H
#define ENTELEA_CAPABILITY_H

/* R04 capability registration contract: see
 * framework/execution/capability/contract.md.
 * Peripheral descriptors (static registration; rollback leaves no partial
 * registration) plus on-demand capability checks. DETACH semantics go through
 * safety force_channel_fault (the existing mechanism surface). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>
#include <entelea/safety.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_PERIPH_DESCS_MAX 24 /* DEC-27 #7 (matches legacy limit) */

typedef enum {
	ENTELEA_DEV_OUTPUT = 0, /* output via protection layer (safety uid) */
	ENTELEA_DEV_INPUT, /* input, bypasses protection layer (contract 3) */
	ENTELEA_DEV_SENSOR,
	ENTELEA_DEV_KIND_COUNT,
} entelea_dev_kind_t;

typedef struct {
	const char *uid; /* logical name (frozen at registration) */
	entelea_dev_kind_t kind;
	uint8_t inst; /* instance no. (<= 31, matches capability bitmap) */
	entelea_perm_class_t perm_class;
	const char *safety_uid; /* output channel's safety uid (NULL = none) */
} entelea_periph_desc_t;

/** [boot] Registers a batch atomically. If any entry is invalid, has a name
 * collision, or exceeds the limit, all entries are rolled back (E_PARAM). */
entelea_result_t
entelea_periph_register(const entelea_periph_desc_t *descs, size_t count);

/** [any] Finds a descriptor by uid (NULL = not registered). */
const entelea_periph_desc_t *
entelea_periph_find(const char *uid);

/** [any] Number of registered descriptors. */
size_t
entelea_periph_count(void);

/** [any] Capability gate and output channel address. Checks the principal's
 * class/instance authorization for the descriptor. Output devices must also
 * have their safety channel present and committable. A direct safety commit
 * that bypasses this gate is a caller violation (review surface). */
entelea_result_t
entelea_periph_acquire(entelea_principal_t ctx, const char *uid,
		       const entelea_periph_desc_t **out);

/** [boot/test] DETACH: if the device is physically absent, raises a single
 * safety-channel fault (existing mechanism surface). The descriptor is kept;
 * reattaching goes through entelea_safety_channel_recover. */
entelea_result_t
entelea_periph_detach(const char *uid);

#ifdef ENTELEA_TEST
void
entelea_periph_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_CAPABILITY_H */
