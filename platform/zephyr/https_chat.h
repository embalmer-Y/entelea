/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HTTPS_CHAT_ZEPHYR_H
#define ENTELEA_HTTPS_CHAT_ZEPHYR_H

/* r15 板上模型传输（与 platform/linux/https_chat.h 同源合同——F-2
 * host 面闭环后的 MCU 面移植；差异面：CA=嵌入根数组（板无文件系统）、
 * socket=Zephyr POSIX、RNG=Zephyr mbedtls 模块 PSA）。凭据经调用方
 * 注入（P4C2 串口协议），不落盘不打日志。 */

#include <stddef.h>
#include <stdint.h>
#include <entelea/model.h>
#include <entelea/result.h>

#define ENTELEA_HTTPS_BODY_MAX 8192 /* 板上面（max_tokens 256 响应界） */

typedef struct {
	char host[64];       /* API 主机名（DNS 解析+SNI） */
	char path_prefix[64];
	char api_key[128];
	char model[48];
	int *touched;        /* 调用计数（闸判据） */
} entelea_https_cfg_t;

/** POST {path_prefix}/chat/completions，TLS1.3，值感知提取 content。
 * 合同与 host 版一致：401/403→E_PERM、429→E_BUSY、截断显式 E_RANGE。 */
entelea_result_t entelea_https_chat(void *user,
				    const entelea_model_msg_t *msgs, uint8_t n,
				    char *out, size_t out_max,
				    uint32_t timeout_ms,
				    entelea_model_usage_t *usage);

#endif /* ENTELEA_HTTPS_CHAT_ZEPHYR_H */
