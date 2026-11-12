/* SPDX-License-Identifier: Apache-2.0 */
/*
 * prov maintenance write channel and flash-test injection (migrated from
 * the old store/prov_maint.c and prov_test.c; R03 contract:
 * framework/services/config/contract.md).
 * prov.c itself has no writes and stays unchanged (mechanical check
 * target). Maintenance window: a full schema check before writing
 * (fail-closed: a failure writes nothing), plus a one-shot window per boot
 * (RAM flag: writing again requires a reset cycle, so remote identity
 * changes are structurally rate-limited and observable). The record format
 * matches flash injection (header len|crc followed by CBOR in one contiguous
 * write, per the single-program flash discipline).
 */
#include <string.h>
#include <entelea/prov.h>
#include <entelea/storage.h>
#include "internal.h"

static bool maint_written_this_boot;

entelea_result_t
entelea_prov_write_maint(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len + 6 > 4096) {
		return ENTELEA_E_PARAM;
	}
	if (maint_written_this_boot) {
		return ENTELEA_E_STATE; /* One-shot: reset to write again */
	}
	entelea_result_t v = entelea_prov_validate(cbor, len);

	if (v != ENTELEA_OK) {
		return v; /* Invalid prov: zero writes (fail-closed) */
	}
	static uint8_t rec[4096];

	entelea_cfg_put_le32(rec, len);
	entelea_cfg_put_le16(rec + 4, entelea_cfg_crc16(cbor, len));
	memcpy(rec + 6, cbor, len);
	if (entelea_storage_erase(ENTELEA_PART_PROV) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_storage_write(ENTELEA_PART_PROV, 0, rec, 6 + len) !=
	    ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	maint_written_this_boot = true;
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void
entelea_prov_maint_test_reset(void)
{
	maint_written_this_boot = false;
}

entelea_result_t
entelea_prov_write_test(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len + 6 > 4096) {
		return ENTELEA_E_PARAM;
	}
	static uint8_t rec[4096];

	entelea_cfg_put_le32(rec, len);
	entelea_cfg_put_le16(rec + 4, entelea_cfg_crc16(cbor, len));
	memcpy(rec + 6, cbor, len);
	if (entelea_storage_erase(ENTELEA_PART_PROV) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_storage_write(ENTELEA_PART_PROV, 0, rec, 6 + len) !=
	    ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}
#endif
