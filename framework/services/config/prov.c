/* SPDX-License-Identifier: Apache-2.0 */
/*
 * provisioning loader (migrated from the old store/prov.c; R03 contract:
 * framework/services/config/contract.md).
 * PROV partition layout (little-endian): u32 cbor_len | u16 crc16(cbor) |
 * cbor[]. Schema v1 (9 pairs, fixed order): v/node_id/cube_id/
 * routers[tstr, <=4]/pk0/pk1/cred/pwr_ma/estop. v2 = v1 prefix plus appended
 * tail (11 pairs: deploy_mode + estop_remote_clear; 12 pairs: + lconf; 15
 * pairs: + tls_ca/tls_cert/tls_key; the three TLS keys are all present or
 * all absent, and a len=0 key is structural damage). Deterministic subset
 * decoding: superset, indefinite length or out-of-domain values = E_IO
 * (fail-closed, never silently merged). Missing fields default to
 * orchestrated / remote clear open / lconf 0 / TLS off (existing prov stays
 * compatible without reflashing).
 * This file makes no write calls (write channels: the maintenance window
 * (maint) or flash injection prov_write_test, kept separate from prov.c as a
 * mechanical check target). The TLS triple has been resident in the loaded
 * state since R03 (the old CONFIG_TS_NET_TLS gate is re-checked for footprint
 * when the R06 consumer is wired in).
 */
#include <string.h>
#include <entelea/prov.h>
#include <entelea/storage.h>
#include "internal.h"

#define PROV_CBOR_MAX 3800U /* Structural cap within the 4KiB partition */

/* Fallback when unloaded or the load failed: same meaning as v1
 * (estop_remote_clear open; test builds often have no prov). On a production
 * board a failed load goes through boot fail-safe; this value only gives the
 * safe-side view before the failure. */
static entelea_prov_t prov = {
	.deploy_mode = ENTELEA_DEPLOY_ORCHESTRATED,
	.estop_remote_clear = 1,
};

static entelea_prov_tls_t prov_tls;

static entelea_result_t
prov_walk(const uint8_t *cbor, uint32_t cbor_len, entelea_prov_t *out,
	  entelea_prov_tls_t *tls_out);

entelea_result_t
entelea_prov_load(void)
{
	uint8_t hdr[6];

	if (entelea_storage_read(ENTELEA_PART_PROV, 0, hdr, sizeof(hdr)) !=
	    ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint32_t cbor_len = entelea_cfg_get_le32(hdr);
	uint16_t crc = entelea_cfg_get_le16(hdr + 4);

	if (cbor_len == 0 || cbor_len > PROV_CBOR_MAX) {
		return ENTELEA_E_IO;
	}
	static uint8_t cbor[PROV_CBOR_MAX];

	if (entelea_storage_read(ENTELEA_PART_PROV, sizeof(hdr), cbor,
				 cbor_len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_cfg_crc16(cbor, cbor_len) != crc) {
		return ENTELEA_E_IO; /* CRC fail: caller fail-safe */
	}
	return prov_walk(cbor, cbor_len, &prov, &prov_tls);
}

static entelea_result_t
prov_walk(const uint8_t *cbor, uint32_t cbor_len, entelea_prov_t *out,
	  entelea_prov_tls_t *tls_out)
{
	entelea_rd_t r = { .p = cbor, .rem = cbor_len };
	uint8_t ib;

	if (!entelea_rd_u8(&r, &ib) || (uint8_t) (ib & 0xE0) != 0xA0) {
		return ENTELEA_E_IO;
	}
	uint32_t pairs;

	/* v1: 9 pairs; v2: 11/12/15 pairs (v1 prefix + appended tail). */
	if (!entelea_rd_len(&r, ib & 0x1F, &pairs) ||
	    (pairs != 9 && pairs != 11 && pairs != 12 && pairs != 15)) {
		return ENTELEA_E_IO;
	}

	/* Defaults first, then the walk (out may be dirty scratch). */
	out->deploy_mode = ENTELEA_DEPLOY_ORCHESTRATED;
	out->estop_remote_clear = 1;
	out->local_confirm = 0;
	out->tls_ca_len = 0;
	out->tls_cert_len = 0;
	out->tls_key_len = 0;

	static const char *const keys[15] = {
		"v",
		"node_id",
		"cube_id",
		"routers",
		"pk0",
		"pk1",
		"cred",
		"pwr_ma",
		"estop",
		"deploy_mode",
		"estop_remote_clear",
		"lconf",
		"tls_ca",
		"tls_cert",
		"tls_key",
	};
	char key[20];
	uint32_t u;

	for (uint32_t k = 0; k < pairs; k++) {
		if (!entelea_rd_tstr(&r, key, sizeof(key)) ||
		    strcmp(key, keys[k]) != 0) {
			return ENTELEA_E_IO;
		}
		switch (k) {
		case 0: /* 9 pairs = v1, 11/12/15 = v2 (mismatch: corrupt) */
			if (!entelea_rd_uint(&r, &u) ||
			    u != (uint32_t) (pairs == 9 ? 1 : 2)) {
				return ENTELEA_E_IO;
			}
			break;
		case 1:
			if (!entelea_rd_tstr(&r, out->node_id,
					     sizeof(out->node_id))) {
				return ENTELEA_E_IO;
			}
			break;
		case 2:
			if (!entelea_rd_tstr(&r, out->cube_id,
					     sizeof(out->cube_id))) {
				return ENTELEA_E_IO;
			}
			break;
		case 3: { /* routers: definite array of tstr, <=4 */
			uint8_t ab;

			if (!entelea_rd_u8(&r, &ab) ||
			    (uint8_t) (ab & 0xE0) != 0x80) {
				return ENTELEA_E_IO;
			}
			uint32_t n;

			if (!entelea_rd_len(&r, ab & 0x1F, &n) || n > 4) {
				return ENTELEA_E_IO;
			}
			for (uint32_t i = 0; i < n; i++) {
				if (!entelea_rd_tstr(
					    &r, out->router_locators[i],
					    sizeof(out->router_locators[i]))) {
					return ENTELEA_E_IO;
				}
			}
			for (uint32_t i = n; i < 4; i++) {
				out->router_locators[i][0] = '\0';
			}
			break;
		}
		case 4:
			if (!entelea_rd_bstr(&r, out->root_pubkeys[0], 32)) {
				return ENTELEA_E_IO;
			}
			break;
		case 5:
			if (!entelea_rd_bstr(&r, out->root_pubkeys[1], 32)) {
				return ENTELEA_E_IO;
			}
			break;
		case 6:
			if (!entelea_rd_tstr(&r, out->zenoh_cred,
					     sizeof(out->zenoh_cred))) {
				return ENTELEA_E_IO;
			}
			break;
		case 7:
			if (!entelea_rd_uint(&r, &out->power_budget_ma)) {
				return ENTELEA_E_IO;
			}
			break;
		case 8: /* estop flag field, u8 (no silent truncation) */
			if (!entelea_rd_uint(&r, &u) || u > 0xFFU) {
				return ENTELEA_E_IO;
			}
			out->estop_trigger_flags = (uint8_t) u;
			break;
		case 9: /* deploy_mode: out-of-domain value fails closed */
			if (!entelea_rd_uint(&r, &u) ||
			    u > (uint32_t) ENTELEA_DEPLOY_STANDALONE) {
				return ENTELEA_E_IO;
			}
			out->deploy_mode = (uint8_t) u;
			break;
		case 10: /* estop_remote_clear: not 0|1 = fail-closed */
			if (!entelea_rd_uint(&r, &u) || u > 1U) {
				return ENTELEA_E_IO;
			}
			out->estop_remote_clear = (uint8_t) u;
			break;
		default:
			if (k == 11) { /* lconf: fail-closed outside 0|1 */
				if (!entelea_rd_uint(&r, &u) || u > 1U) {
					return ENTELEA_E_IO;
				}
				out->local_confirm = (uint8_t) u;
				break;
			}
			/* 12/13/14: TLS DER bstr; tls_out=NULL is the
			 * zero-copy validate walk. */
			uint16_t blen = 0;
			uint8_t *dst = NULL;
			size_t cap = 0;
			uint16_t *dlen = NULL;

			switch (k) {
			case 12:
				dst = tls_out ? tls_out->ca : NULL;
				cap = ENTELEA_PROV_TLS_CA_MAX;
				dlen = &out->tls_ca_len;
				break;
			case 13:
				dst = tls_out ? tls_out->cert : NULL;
				cap = ENTELEA_PROV_TLS_CERT_MAX;
				dlen = &out->tls_cert_len;
				break;
			default:
				dst = tls_out ? tls_out->key : NULL;
				cap = ENTELEA_PROV_TLS_KEY_MAX;
				dlen = &out->tls_key_len;
				break;
			}
			if (!entelea_rd_bstr_len(&r, dst, cap, &blen) ||
			    blen == 0) {
				return ENTELEA_E_IO; /* empty value: corrupt */
			}
			*dlen = blen;
			if (tls_out != NULL) {
				/* Mirror the length in the loaded state:
				 * the single read surface for consumers. */
				if (k == 12) {
					tls_out->ca_len = blen;
				} else if (k == 13) {
					tls_out->cert_len = blen;
				} else {
					tls_out->key_len = blen;
				}
			}
			break;
		}
	}
	if (r.rem != 0) {
		return ENTELEA_E_IO; /* trailing bytes: structural damage */
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_prov_validate(const uint8_t *cbor, uint32_t len)
{
	if (cbor == NULL || len == 0 || len > PROV_CBOR_MAX) {
		return ENTELEA_E_PARAM;
	}
	entelea_prov_t scratch;

	/* Zero-copy walk (tls_out=NULL): validate never touches the
	 * live loaded state. */
	return prov_walk(cbor, len, &scratch, NULL);
}

const entelea_prov_t *
entelea_prov(void)
{
	return &prov;
}

const entelea_prov_tls_t *
entelea_prov_tls(void)
{
	return &prov_tls;
}
