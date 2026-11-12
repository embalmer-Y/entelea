/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_PROV_H
#define ENTELEA_PROV_H

/* R03 provisioning configuration contract: see
 * framework/services/config/contract.md.
 * Read-only at runtime (the write path is only the flashing or maintenance
 * window). The load path makes zero write calls (a mechanically checked goal).
 * CBOR schema v1/v2 (key order: v1 prefix, new keys appended at the tail;
 * deterministic subset decoding). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ENTELEA_DEPLOY_ORCHESTRATED = 0, /* link loss -> SAFE_LINKLOSS; default
					    when the field is missing */
	ENTELEA_DEPLOY_STANDALONE = 1, /* standalone: link loss is only logged,
					  no migration */
} entelea_deploy_mode_t;

typedef struct {
	char node_id[16];
	char cube_id[16];
	char router_locators[4][48];
	uint8_t root_pubkeys[2][32];
	char zenoh_cred[96];
	uint32_t power_budget_ma;
	uint8_t estop_trigger_flags;
	/* schema v2 fields, appended at the tail (defaults when absent:
	 * orchestrated, remote clear allowed, lconf 0) */
	uint8_t deploy_mode; /* entelea_deploy_mode_t */
	uint8_t estop_remote_clear; /* 0/1 */
	uint8_t local_confirm; /* 0/1: install-gate local confirmation */
	uint16_t tls_ca_len;
	uint16_t tls_cert_len;
	uint16_t tls_key_len; /* 0 = TLS off; all 3 keys present or none */
} entelea_prov_t;

/* TLS DER limits (P-256 measured, +~30% margin, DEC-63 #3). */
#define ENTELEA_PROV_TLS_CA_MAX 768
#define ENTELEA_PROV_TLS_CERT_MAX 768
#define ENTELEA_PROV_TLS_KEY_MAX 256

typedef struct {
	uint8_t ca[ENTELEA_PROV_TLS_CA_MAX];
	uint8_t cert[ENTELEA_PROV_TLS_CERT_MAX];
	uint8_t key[ENTELEA_PROV_TLS_KEY_MAX];
	uint16_t ca_len;
	uint16_t cert_len;
	uint16_t key_len;
} entelea_prov_tls_t;

/** [boot/consumer] Loads the config (header len|crc16 + CBOR). A schema check
 * or CRC failure returns ENTELEA_E_IO; the caller then goes fail-safe.
 * Uses the same parsing path as validate. */
entelea_result_t
entelea_prov_load(void);

/** [any] Read-only pointer. If not loaded or the load failed, returns safe-side
 * values equivalent to v1. There is no runtime write interface. */
const entelea_prov_t *
entelea_prov(void);

/** [any] Read-only pointer to the TLS triple (len = 0 means TLS off). */
const entelea_prov_tls_t *
entelea_prov_tls(void);

/** [thread] Validates a maintenance-channel write without touching the live
 * loaded state (zero-copy walk-through). */
entelea_result_t
entelea_prov_validate(const uint8_t *cbor, uint32_t len);

/** [thread] Maintenance write: validates first (a failure writes nothing).
 * It then uses a one-shot window per boot. A further write needs a reset
 * cycle, which structurally rate-limits remote identity changes and keeps
 * them observable. */
entelea_result_t
entelea_prov_write_maint(const uint8_t *cbor, uint32_t len);

#ifdef ENTELEA_TEST
/** Test injection for the flashing channel (test builds only; production
 * flashing uses external tools). */
entelea_result_t
entelea_prov_write_test(const uint8_t *cbor, uint32_t len);
/** Resets the maintenance-window RAM flag (test isolation simulates a reset
 * cycle). */
void
entelea_prov_maint_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_PROV_H */
