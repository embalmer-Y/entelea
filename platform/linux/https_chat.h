/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HTTPS_CHAT_H
#define ENTELEA_HTTPS_CHAT_H
/* F-2 host surface model transport: the real implementation of
 * entelea_model_transport_t.chat. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/model.h>

#define ENTELEA_HTTPS_BODY_MAX 16384

typedef struct {
	char host[64];
	char path_prefix[64]; /* e.g. "/api/v1" */
	char api_key[128];
	char model[48];
	char ca_bundle[96]; /* Empty = system default bundle */
	int *touched; /* Transport touch count (refusal leaves it unchanged) */
} entelea_https_cfg_t;

entelea_result_t
entelea_https_chat(void *user, const entelea_model_msg_t *msgs, uint8_t n,
		   char *out, size_t out_max, uint32_t timeout_ms,
		   entelea_model_usage_t *usage);

#endif /* ENTELEA_HTTPS_CHAT_H */
