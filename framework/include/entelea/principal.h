/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PRINCIPAL_H
#define ENTELEA_PRINCIPAL_H

/* R04 trusted caller and capability decision contract: see
 * framework/execution/capability/contract.md.
 * Only the trusted host can bind and pass a principal; integers from Wasm or
 * Lua input cannot be turned into a principal. The public C struct is not an
 * unforgeable token; within the same address space it relies on the host trust
 * boundary. The capability grammar is class:op:instances (numeric bitmap) and
 * msg:name list (P1-1). Unauthorized access is always refused and recorded
 * (EVT_PERM_DENIED). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ENTELEA_PERM_GPIO = 0,
	ENTELEA_PERM_PWM,
	ENTELEA_PERM_ADC,
	ENTELEA_PERM_POWER,
	ENTELEA_PERM_FS,
	ENTELEA_PERM_AV,
	ENTELEA_PERM_LOG,
	ENTELEA_PERM_KV,
	ENTELEA_PERM_MSG,
	ENTELEA_PERM_SYS,
	ENTELEA_PERM_CLASS_COUNT,
} entelea_perm_class_t;

typedef enum {
	ENTELEA_PERM_READ = 0,
	ENTELEA_PERM_WRITE,
	ENTELEA_PERM_SET,
	ENTELEA_PERM_LIST,
	ENTELEA_PERM_DELETE,
	ENTELEA_PERM_SEND,
	ENTELEA_PERM_OP_COUNT,
} entelea_perm_op_t;

#define ENTELEA_PERM_MSG_NAME_MAX 40
#define ENTELEA_PERM_MSG_TARGETS_MAX 4

typedef struct {
	uint32_t grant[ENTELEA_PERM_CLASS_COUNT][ENTELEA_PERM_OP_COUNT];
	char msg_targets[ENTELEA_PERM_MSG_TARGETS_MAX]
			[ENTELEA_PERM_MSG_NAME_MAX];
} entelea_perm_table_t;

/* context.h forward declaration. The full definition is public here so that
 * the handle can be passed by value. bind is the construction entry point
 * agreed on by the trusted host, not a C-level unforgeability guarantee. */
#include <entelea/context.h>

struct entelea_principal {
	uint8_t _handle; /* 0 = unbound; 1..MAX = slot + 1 */
	uint16_t _gen; /* bind generation of the slot (non-zero; anti-ABA) */
	uint16_t app_id;
};

/** Initializes the capability table (all zeros = no authorization). */
void
entelea_perm_table_init(entelea_perm_table_t *table);

/** [host] Parses and merges one capability declaration (a class:op:instances
 * bitmap, or msg:send:name). An out-of-range class or operation, an instance
 * above 31, or bad grammar returns E_PARAM (fail-closed; never silently
 * truncated). Duplicate declarations are idempotent. */
entelea_result_t
entelea_perm_parse(const char *cap, entelea_perm_table_t *table);

/** [host] Parses a semicolon-separated declaration list. Empty means no
 * declarations (OK). An empty, over-long, or malformed segment returns
 * E_PARAM. After a failure the table contents are undefined; discard them. */
entelea_result_t
entelea_perm_parse_list(const char *list, size_t len,
			entelea_perm_table_t *table);

/** [host] Binds a principal (up to 4 slots; an app_id collision is refused).
 * Unbinding invalidates the handle: any later decision on an old handle,
 * including a copy the host saved elsewhere, returns E_PERM. A handle must
 * match the app_id and generation currently bound to its slot. Once a slot's
 * generation is exhausted, it is not reused in this run. With no free slot,
 * returns E_NOMEM. A handle is valid only for the current run; it must not be
 * persisted or reused across reboots. */
entelea_result_t
entelea_principal_bind(entelea_principal_t *ctx, uint16_t app_id,
		       const entelea_perm_table_t *table);

/** [host] Unbinds (appmgr stop chain). Takes effect only if ctx is the slot's
 * current binding. Unbinding with a stale copy is a no-op and must not revoke a
 * newer binding in the same slot. */
void
entelea_principal_unbind(entelea_principal_t *ctx);

/** [any] Decision: an undeclared permission, an unbound handle, or a handle
 * field mismatch returns E_PERM and records an attempt event. Bind, unbind, and
 * decisions are synchronized; a successful decision does not guarantee that the
 * permission remains unrevoked during later actions. */
entelea_result_t
entelea_perm_check(entelea_principal_t ctx, entelea_perm_class_t cls,
		   entelea_perm_op_t op, uint8_t inst);

/** [any] Decides against the msg list (send only; recv is passive and has no
 * permission gate). The name must follow the declaration grammar and terminate
 * within 40 bytes, otherwise E_PERM. NULL returns E_PARAM. */
entelea_result_t
entelea_perm_check_msg(entelea_principal_t ctx, const char *target);

#ifdef ENTELEA_TEST
void
entelea_perm_test_reset(void);
void
entelea_perm_test_after_resolve(void (*hook)(void));
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PRINCIPAL_H */
