/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HTTPS_CHAT_H
#define ENTELEA_HTTPS_CHAT_H
/* F-2 host 面模型传输（entelea_model_transport_t.chat 的真实现）。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/model.h>

#define ENTELEA_HTTPS_BODY_MAX 16384

typedef struct {
	char host[64];
	char path_prefix[64]; /* 如 "/api/v1" */
	char api_key[128];
	char model[48];
	char ca_bundle[96]; /* 空=系统缺省 bundle */
	int *touched;        /* 传输触碰计数（联动拒的"不触传输"证据） */
} entelea_https_cfg_t;

entelea_result_t entelea_https_chat(void *user,
				    const entelea_model_msg_t *msgs, uint8_t n,
				    char *out, size_t out_max,
				    uint32_t timeout_ms,
				    entelea_model_usage_t *usage);

#endif /* ENTELEA_HTTPS_CHAT_H */
