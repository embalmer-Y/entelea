/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_APPSTORE_H
#define ENTELEA_APPSTORE_H

/* R05 package facts, verification and loading contract: see
 * framework/services/package/contract.md.
 * Responsibilities: this layer manages package facts (staging -> verify ->
 * activate) and signature verification. The execution loop runs in the WAMR
 * host (wasm_host.c); trial policy belongs to R12. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/package.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_APP_SLOT_COUNT 2
#define ENTELEA_APP_SLOT_SIZE 262144U
#define ENTELEA_TRUST_SET_MAX 10 /* factory 2 + trust table 8 */

/* ---- Step-wise install (MA3.1 semantics): staging, verify, activate ---- */

/** Begins staging (erases the slot; slot is in [0, COUNT)). */
entelea_result_t
entelea_pkg_stage_begin(uint8_t slot);

/** Appends a chunk (sequential write with read-back; rejects out-of-bounds
 * or out-of-order writes). */
entelea_result_t
entelea_pkg_stage_chunk(uint8_t slot, uint32_t off, const void *buf,
			uint32_t len);

/** Full verification: TSAP v2 header, sha256 digest comparison, COSE_Sign1
 * ed25519 (tweetnacl), and comparison with the embedded copy (prevents signing
 * A but installing B). root=NULL means the effective trust set (factory root
 * union the R03 trust table). Format or signature errors return
 * E_INVALID_SIG; storage read failures return E_IO. A failure does not
 * activate. In ENTELEA_TEST builds the verification object is the fixed test
 * root (public material; test trust is not production trust). */
entelea_result_t
entelea_pkg_verify(uint8_t slot, const uint8_t root_pubkey[32],
		   entelea_tsap_view_t *v, uint8_t digest_out[32]);

/** Whole-file digest: sha256(slot[0, len)). len is taken from the verification
 * view's pkg_len (the Forge package_digest input domain). An out-of-domain slot
 * or len returns E_PARAM; a read failure returns E_IO. It shares the global
 * sha256 stream context, so it must not be interleaved with other streams
 * (single-thread assumption). */
entelea_result_t
entelea_pkg_file_digest(uint8_t slot, uint32_t len, uint8_t out[32]);

/** Single-pass capture: one streaming read computes sha256(slot[0, len)) and
 * copies slot[cap_off, cap_off+cap_len) into cap_buf (no capture when
 * cap_len=0). The load chain uses this so that the digest checked by the gate
 * and the bytes actually loaded come from the same read. An out-of-domain
 * request (including a range beyond len) returns E_PARAM; a read failure
 * returns E_IO. */
entelea_result_t
entelea_pkg_file_digest_capture(uint8_t slot, uint32_t len, uint8_t out[32],
				uint32_t cap_off, uint32_t cap_len,
				uint8_t *cap_buf);

/** Activates (records the active slot in KV "PAGA"). This is only a **package
 * fact record** and grants no execution right. Before loading, the load chain
 * (wasm_host) re-checks verification and the R12 trial gate. For stable-set
 * admission and promotion, see the stable record in delivery.h (D-E).
 * Activation alone grants no permission. */
entelea_result_t
entelea_pkg_activate(uint8_t slot);

/** Current active slot. Returns E_UNINIT if there is none; media and init
 * errors are propagated unchanged. */
entelea_result_t
entelea_pkg_active(uint8_t *slot);

/** Direct slot read: the staged package-fact read surface. The load chain
 * (wasm_host) uses this production path to fetch the wasm segment. The D-3
 * gate first discovered, while compiling wasm_host in a non-TEST build, that
 * this had been wrongly placed inside a TEST block. */
entelea_result_t
entelea_pkg_slot_read(uint8_t slot, uint32_t off, void *buf, uint32_t len);

#ifdef ENTELEA_TEST
/* Fixed test root (public material; the generator script and private key are
 * in the legacy Forge fixtures). */
extern const uint8_t entelea_pkg_test_root_pub[32];
/* Direct slot write (test injection surface for corruption and out-of-bounds
 * oracles; not a production path). */
entelea_result_t
entelea_pkg_slot_write(uint8_t slot, uint32_t off, const void *buf,
		       uint32_t len);
entelea_result_t
entelea_pkg_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_APPSTORE_H */
