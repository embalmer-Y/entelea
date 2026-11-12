/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HTTPS_CHAT_ZEPHYR_H
#define ENTELEA_HTTPS_CHAT_ZEPHYR_H

/* r15 on-board model transport (same contract as platform/linux/https_chat.h;
 * the MCU-side port after the F-2 host-side loop closed. Differences: CA = an
 * embedded root array (the board has no filesystem), socket = Zephyr POSIX,
 * RNG = Zephyr mbedtls module PSA). Credentials are injected by the caller
 * (P4C2 serial protocol); they are never written to disk or logged. */

#include <stddef.h>
#include <stdint.h>
#include <entelea/model.h>
#include <entelea/result.h>

#define ENTELEA_HTTPS_BODY_MAX 8192 /* board bound: max_tokens 256 response */

typedef struct {
	char host[64]; /* API hostname (DNS resolution + SNI) */
	char path_prefix[64];
	char api_key[128];
	char model[48];
	int *touched; /* call counter (gate criterion) */
} entelea_https_cfg_t;

/** POST {path_prefix}/chat/completions over TLS 1.3, with value-aware
 * content extraction. Matches the host version's contract: 401/403 -> E_PERM,
 * 429 -> E_BUSY, truncation explicitly E_RANGE. */
entelea_result_t
entelea_https_chat(void *user, const entelea_model_msg_t *msgs, uint8_t n,
		   char *out, size_t out_max, uint32_t timeout_ms,
		   entelea_model_usage_t *usage);

#endif /* ENTELEA_HTTPS_CHAT_ZEPHYR_H */
