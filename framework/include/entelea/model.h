/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_MODEL_H
#define ENTELEA_MODEL_H

/* R08 模型适配层（B-2；合同 framework/services/assistant/contract.md
 * "模型适配层"节）。框架面=策略层：依赖联动（模型/网络任一 DOWN 拒
 * 调用——D08）、有界重试（仅确定请求未发送的 E_NOT_SENT；E_IO
 * 和 E_TIMEOUT 的效果可能未知，不自动重试）、用量累计。传输=可插拔 ops（测试
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

#define ENTELEA_MODEL_RETRIES 2U /* 已确认未发送时的重试上限 */
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
	/** 单次对话补全。只有能确定未发送应用请求时才可返回
	 * E_NOT_SENT；任何可能已发送的失败须返回 E_IO/E_TIMEOUT 等。
	 * usage 尽力填充（0=未知）。 */
	entelea_result_t (*chat)(void *user, const entelea_model_msg_t *msgs,
				 uint8_t n, char *out, size_t out_max,
				 uint32_t timeout_ms,
				 entelea_model_usage_t *usage);
} entelea_model_transport_t;

/** [boot] 绑定传输（NULL=注销——ask 拒）。 */
void entelea_model_bind(const entelea_model_transport_t *t, void *user);

/** [any] 就绪 = 已绑传输、模型/网络 UP 且风暴闩未锁。 */
bool entelea_model_ready(void);

/** [thread] 提问（策略层）：未绑/依赖 DOWN=E_STATE（不触传输）；
 * 仅 E_NOT_SENT 重试至 ENTELEA_MODEL_RETRIES；E_IO/E_TIMEOUT 等不自动
 * 重试；usage 逐调用累计
 * （usage_out=自启动累计值，单次用量由调用方取差）。每次传输尝试
 * 前过 D12 闸、后逐次上报：闸拒即停（返回 E_BUSY/E_STATE，不触传输，
 * 不计风暴）。 */
entelea_result_t entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n,
				   char *out, size_t out_max,
				   uint32_t timeout_ms,
				   entelea_model_usage_t *usage_out);

/** [thread] 显式恢复探测：网络须 UP，仍经相同 D12 闸；允许模型
 * 尚未 UP 时触发。一次成功仅是一条真实观测，调用方负责向依赖
 * 状态机上报；不得把一次调用伪报成多次成功。 */
entelea_result_t entelea_model_probe(const entelea_model_msg_t *msgs, uint8_t n,
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
