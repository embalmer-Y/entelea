/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Peripheral descriptor registry (migrated from the old periph/desc.c
 * semantics; R04 contract in contract.md of this directory). Static batch
 * registration at boot: a failed batch leaves no partial registration.
 * DETACH = single-channel fault in safety (the mechanism is reused; physical
 * effect semantics are in safety contract A2).
 */
#include <string.h>
#include <entelea/capability.h>

static const entelea_periph_desc_t *descs[ENTELEA_PERIPH_DESCS_MAX];
static size_t desc_count;

static bool
desc_valid(const entelea_periph_desc_t *d)
{
	if (d == NULL || d->uid == NULL || d->uid[0] == '\0' ||
	    (int) d->kind < 0 || d->kind >= ENTELEA_DEV_KIND_COUNT ||
	    (int) d->perm_class < 0 ||
	    d->perm_class >= ENTELEA_PERM_CLASS_COUNT || d->inst > 31) {
		return false;
	}
	if (d->kind == ENTELEA_DEV_OUTPUT && d->safety_uid == NULL) {
		return false; /* Outputs must have a protection channel */
	}
	return true;
}

static int
find_idx(const char *uid)
{
	if (uid == NULL) {
		return -1;
	}
	for (size_t i = 0; i < desc_count; i++) {
		if (strcmp(descs[i]->uid, uid) == 0) {
			return (int) i;
		}
	}
	return -1;
}

const entelea_periph_desc_t *
entelea_periph_find(const char *uid)
{
	int i = find_idx(uid);

	return i < 0 ? NULL : descs[i];
}

size_t
entelea_periph_count(void)
{
	return desc_count;
}

entelea_result_t
entelea_periph_register(const entelea_periph_desc_t *list, size_t count)
{
	if (list == NULL && count > 0) {
		return ENTELEA_E_PARAM;
	}
	if (desc_count + count > ENTELEA_PERIPH_DESCS_MAX) {
		return ENTELEA_E_NOMEM; /* Reject whole batch (predictable) */
	}
	/* Validate the whole batch first (including duplicate uids within
	 * the batch); any failure registers nothing. */
	for (size_t i = 0; i < count; i++) {
		if (!desc_valid(&list[i])) {
			return ENTELEA_E_PARAM;
		}
		if (find_idx(list[i].uid) >= 0) {
			return ENTELEA_E_PARAM; /* uid already registered */
		}
		for (size_t j = 0; j < i; j++) {
			if (strcmp(list[i].uid, list[j].uid) == 0) {
				return ENTELEA_E_PARAM; /* Batch duplicate */
			}
		}
	}
	for (size_t i = 0; i < count; i++) {
		descs[desc_count++] = &list[i];
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_periph_acquire(entelea_principal_t ctx, const char *uid,
		       const entelea_periph_desc_t **out)
{
	if (out != NULL) {
		*out = NULL;
	}
	const entelea_periph_desc_t *d = entelea_periph_find(uid);

	if (d == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	entelea_result_t r = entelea_perm_check(ctx, d->perm_class,
						d->kind == ENTELEA_DEV_OUTPUT ?
							ENTELEA_PERM_WRITE :
							ENTELEA_PERM_READ,
						d->inst);

	if (r != ENTELEA_OK) {
		return r; /* Denied; logged in perm_check */
	}
	if (d->kind == ENTELEA_DEV_OUTPUT) {
		entelea_ch_state_t st;

		if (entelea_safety_channel_state(d->safety_uid, &st) !=
		    ENTELEA_OK) {
			return ENTELEA_E_STATE; /* Declared but unregistered */
		}
		if (st != ENTELEA_ST_ACTIVE) {
			return ENTELEA_E_STATE; /* Safe state: no commit */
		}
	}
	if (out != NULL) {
		*out = d;
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_periph_detach(const char *uid)
{
	const entelea_periph_desc_t *d = entelea_periph_find(uid);

	if (d == NULL) {
		return ENTELEA_E_NOTFOUND;
	}
	if (d->kind != ENTELEA_DEV_OUTPUT || d->safety_uid == NULL) {
		return ENTELEA_E_STATE; /* No detach for non-outputs */
	}
	return entelea_safety_force_channel_fault(d->safety_uid);
}

#ifdef ENTELEA_TEST
void
entelea_periph_test_reset(void)
{
	memset(descs, 0, sizeof(descs));
	desc_count = 0;
}
#endif
