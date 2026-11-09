/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_MODEL_H
#define ENTELEA_MODEL_H

/* R08 模型适配层（B-2；合同 framework/services/assistant/contract.md
 * "模型适配层"节）。框架面=策略层：依赖联动（模型/网络任一 DOWN 拒
 * 调用——D08）、有界重试（仅瞬态 E_IO；E_TIMEOUT 不自动重试——
 * 成本不可知）、用量累计（预算/台账观测）。传输=可插拔 ops（测试
 * mock；真传输随真网批/SDK 批接入同一形状——框架不含网络栈/TLS）。
 * 推理模型特性（glm-5.3-flash 实测）：响应先耗 reasoning tokens——
 * transport 责任含"截断无 content=E_RANGE"与超时下限放宽。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_MODEL_RETRIES 2U /* E_IO 瞬态重试上限（含首发最多 3 次调用） */
#define ENTELEA_MODEL_OUT_MIN 64U /* 输出缓冲下限（建议值） */

typedef struct {
	const char *role;    /* "system"/"user"/"assistant" */
	const char *content;
} entelea_model_msg_t;

typedef struct {
	uint32_t prompt_tokens;
	uint32_t completion_tokens; /* 含推理 token（推理模型实测语义） */
} entelea_model_usage_t;

typedef struct {
	/** 单次对话补全（OpenAI 兼容形状）。out=NUL 终止；截断无
	 * content/超时/E_IO 显式返回；usage 尽力填充（0=未知）。 */
	entelea_result_t (*chat)(void *user, const entelea_model_msg_t *msgs,
				 uint8_t n, char *out, size_t out_max,
				 uint32_t timeout_ms,
				 entelea_model_usage_t *usage);
} entelea_model_transport_t;

/** [boot] 绑定传输（NULL=注销——ask 拒）。 */
void entelea_model_bind(const entelea_model_transport_t *t, void *user);

/** [any] 就绪 = 已绑传输 且 依赖模型 UP 且 网络 UP（D08 联动）。 */
bool entelea_model_ready(void);

/** [thread] 提问（策略层）：未绑/依赖 DOWN=E_STATE（不触传输）；
 * E_IO 重试至 ENTELEA_MODEL_RETRIES；E_TIMEOUT/E_RANGE/E_PERM 不自动
 * 重试（成本/语义不可知——调用方决策）；usage 逐调用累计。 */
entelea_result_t entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n,
				   char *out, size_t out_max,
				   uint32_t timeout_ms,
				   entelea_model_usage_t *usage_out);

/** [any] 累计用量（观测——预算/台账面）。 */
entelea_model_usage_t entelea_model_usage_total(void);

#ifdef ENTELEA_TEST
void entelea_model_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_MODEL_H */
