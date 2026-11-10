/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R08 模型适配层实现（B-2；合同 assistant/contract.md"模型适配层"）。
 * 策略面：依赖联动（模型+网络）、有界 E_IO 重试、用量累计；传输面
 * 全权归可插拔 ops（mock 测试闭环；真传输随真网批——无网络栈/TLS
 * 入框架）。无定时器：重试为立即有界重发（次数受 ENTELEA_MODEL_
 * RETRIES 封顶——成本可控；退避策略随真机批校准）。
 */
#include <string.h>
#include <entelea/d12.h>
#include <entelea/model.h>
#include <entelea/netmon.h>

static const entelea_model_transport_t *transport;
static void *transport_user;
static entelea_model_usage_t total_usage;

void entelea_model_bind(const entelea_model_transport_t *t, void *user)
{
	transport = t;
	transport_user = user;
}

bool entelea_model_ready(void)
{
	return transport != NULL &&
	       entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_UP &&
	       entelea_dep_state(ENTELEA_DEP_NET) == ENTELEA_DEP_UP;
}

entelea_result_t entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n,
				   char *out, size_t out_max,
				   uint32_t timeout_ms,
				   entelea_model_usage_t *usage_out)
{
	if (msgs == NULL || n == 0 || out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (out_max < ENTELEA_MODEL_OUT_MIN) {
		return ENTELEA_E_RANGE; /* 推理模型输出面：过小必截断 */
	}
	if (transport == NULL) {
		return ENTELEA_E_UNINIT;
	}
	if (!entelea_model_ready()) {
		return ENTELEA_E_STATE; /* 依赖联动：不触传输（D08） */
	}
	{
		/* D12 闸（冷却/频度/风暴）前置——被闸即不触传输。 */
		entelea_result_t gate = entelea_d12_model_allow();

		if (gate != ENTELEA_OK) {
			return gate;
		}
	}
	entelea_result_t r = ENTELEA_E_IO;

	for (uint32_t attempt = 0; attempt <= ENTELEA_MODEL_RETRIES; attempt++) {
		entelea_model_usage_t u = {0};

		if (attempt > 0 && !entelea_model_ready()) {
			/* review P3-5：依赖逐尝试重查——传输回调上报依赖
			 * DOWN 后不再重试触传输（严格 D08 联动）。 */
			return ENTELEA_E_STATE;
		}
		r = transport->chat(transport_user, msgs, n, out, out_max,
				    timeout_ms, &u);
		total_usage.prompt_tokens += u.prompt_tokens;
		total_usage.completion_tokens += u.completion_tokens;
		if (r != ENTELEA_E_IO) {
			break; /* 仅瞬态 E_IO 重试；其余显式上抛不自动重发 */
		}
		out[0] = '\0'; /* 重试前清残影（失败路径不留半答） */
	}
	entelea_d12_model_note(r); /* 终态上报（成功清风暴/E_BUSY 冷却） */
	if (usage_out != NULL) {
		*usage_out = total_usage;
	}
	return r;
}

entelea_model_usage_t entelea_model_usage_total(void)
{
	return total_usage;
}

#ifdef ENTELEA_TEST
void entelea_model_test_reset(void)
{
	transport = NULL;
	transport_user = NULL;
	memset(&total_usage, 0, sizeof(total_usage));
}
#endif
