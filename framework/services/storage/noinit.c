/* SPDX-License-Identifier: Apache-2.0 */
/*
 * noinit trace (migrated from the old store/noinit.c; R03 contract in this
 * directory's contract.md):
 * layout (little-endian): u16 magic | u16 len | u16 crc16 | rec[].
 * fresh semantics: a valid record means an abnormal-reset trace from the
 * last reset exists (fresh=false); empty or damaged means fresh=true.
 * One-shot: erased after reading (prevents a stale trace from being falsely
 * reported across a reset).
 * R02 legacy wiring point: the system_fail reset reason is persisted here
 * through the strong implementation of the safety weak hook.
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define NOINIT_MAGIC 0x5A5AU
#define NOINIT_HDR 6

/* Result consumed at init (last_reset query surface). */
static uint32_t last_reason;
static bool last_present;

void
entelea_storage_last_reset(uint32_t *reason, bool *present)
{
	if (reason != NULL) {
		*reason = last_reason;
	}
	if (present != NULL) {
		*present = last_present;
	}
}

/* Strong implementation of the safety weak hook (weak stub in force.c):
 * records the system_fail reason. Calling context: system_fail (after halt,
 * before shutdown). The RAM backend is safe to write to; a flash backend
 * must recheck that this context can write flash when attached (board batch
 * record). */
void
entelea_safety_record_fail_reason(uint32_t reason)
{
	entelea_storage_noinit_put(&reason, sizeof(reason));
}

void
entelea_storage_noinit_put(const void *rec, uint16_t len)
{
	if (rec == NULL || len == 0 || len > ENTELEA_STORAGE_NOINIT_MAX_REC) {
		return;
	}
	uint8_t buf[NOINIT_HDR + ENTELEA_STORAGE_NOINIT_MAX_REC];

	entelea_put_le16(buf, NOINIT_MAGIC);
	entelea_put_le16(buf + 2, len);
	entelea_put_le16(buf + 4, entelea_crc16(rec, len));
	memcpy(buf + NOINIT_HDR, rec, len);
	(void) entelea_store_backend.erase(ENTELEA_PART_NOINIT);
	(void) entelea_store_backend.write(ENTELEA_PART_NOINIT, 0, buf,
					   NOINIT_HDR + len);
}

entelea_result_t
entelea_storage_noinit_get(void *rec, uint16_t *len, bool *fresh)
{
	if (rec == NULL || len == NULL || fresh == NULL) {
		return ENTELEA_E_PARAM;
	}
	uint8_t hdr[NOINIT_HDR];

	*fresh = true;
	if (entelea_store_backend.read(ENTELEA_PART_NOINIT, 0, hdr,
				       sizeof(hdr)) != ENTELEA_OK) {
		return ENTELEA_OK; /* Read failure: no trace, not a fake OK */
	}
	if (entelea_get_le16(hdr) != NOINIT_MAGIC) {
		return ENTELEA_OK; /* Erased or no trace: fresh */
	}
	uint16_t rlen = entelea_get_le16(hdr + 2);

	if (rlen == 0 || rlen > ENTELEA_STORAGE_NOINIT_MAX_REC) {
		return ENTELEA_OK;
	}
	uint8_t data[ENTELEA_STORAGE_NOINIT_MAX_REC];

	if (entelea_store_backend.read(ENTELEA_PART_NOINIT, NOINIT_HDR, data,
				       rlen) != ENTELEA_OK) {
		return ENTELEA_OK;
	}
	if (entelea_crc16(data, rlen) != entelea_get_le16(hdr + 4)) {
		return ENTELEA_OK; /* Corrupt: treat as no trace */
	}
	memcpy(rec, data, rlen);
	*len = rlen;
	*fresh = false;
	/* One-shot: erase after reading. */
	(void) entelea_store_backend.erase(ENTELEA_PART_NOINIT);
	return ENTELEA_OK;
}

/* Consumed during init (called by the boot steps): if a trace exists, record
 * the reason for query and reporting. Only the u32 reason form (system_fail
 * encoding) is recognised; other lengths are consumed normally, not kept. */
entelea_result_t
entelea_storage_init(void)
{
	uint8_t rec[ENTELEA_STORAGE_NOINIT_MAX_REC];
	uint16_t len = 0;
	bool fresh = true;

	last_present = false;
	last_reason = 0;
	if (entelea_storage_noinit_get(rec, &len, &fresh) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (!fresh && len == sizeof(uint32_t)) {
		last_reason = entelea_get_le32(rec);
		last_present = true;
	}
	/* Audit ring cursor recovery. A missing partition or scan failure is
	 * reported as-is; the caller handles observation-surface downgrade
	 * (the RAM backend never triggers it). */
	return entelea_storage_audit_init();
}
