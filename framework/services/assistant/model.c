/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R08 模型适配层实现（B-2；合同 assistant/contract.md"模型适配层"）。
 * 策略面：依赖联动（模型+网络）、确认未发送时有界重试、用量累计；传输面
 * 全权归可插拔 ops（mock 测试闭环；真传输随真网批——无网络栈/TLS
 * 入框架）。无定时器：重试为立即有界重发（次数受 ENTELEA_MODEL_
 * RETRIES 封顶——成本可控；退避策略随真机批校准）。每次传输尝试
 * 均经 D12 闸并逐次上报（RF1 G5：重试不绕频度/风暴）。
 */
#include <string.h>
#include <entelea/d12.h>
#include <entelea/model.h>
#include <entelea/netmon.h>

static const entelea_model_transport_t *transport;
static void *transport_user;
static entelea_model_usage_t total_usage;

static uint32_t usage_add(uint32_t total, uint32_t value)
{
	return value > UINT32_MAX - total ? UINT32_MAX : total + value;
}

void entelea_model_bind(const entelea_model_transport_t *t, void *user)
{
	transport = t;
	transport_user = user;
}

bool entelea_model_ready(void)
{
	return transport != NULL && transport->chat != NULL &&
	       entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_UP &&
	       entelea_dep_state(ENTELEA_DEP_NET) == ENTELEA_DEP_UP &&
	       !entelea_d12_storm_latched();
}

static entelea_result_t model_call(const entelea_model_msg_t *msgs, uint8_t n,
				    char *out, size_t out_max,
				    uint32_t timeout_ms,
				    entelea_model_usage_t *usage_out,
				    bool probe)
{
	if (msgs == NULL || n == 0 || out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (out_max < ENTELEA_MODEL_OUT_MIN) {
		return ENTELEA_E_RANGE; /* 推理模型输出面：过小必截断 */
	}
	out[0] = '\0';
	if (transport == NULL || transport->chat == NULL) {
		return ENTELEA_E_UNINIT;
	}
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP ||
	    (!probe && !entelea_model_ready())) {
		return ENTELEA_E_STATE; /* 依赖联动：不触传输（D08） */
	}
	entelea_result_t r = ENTELEA_E_IO;

	for (uint32_t attempt = 0; attempt <= ENTELEA_MODEL_RETRIES; attempt++) {
		entelea_model_usage_t u = {0};
		entelea_result_t gate;

		if (attempt > 0 &&
		    (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP ||
		     (!probe && !entelea_model_ready()))) {
			/* review P3-5：依赖逐尝试重查——传输回调上报依赖
			 * DOWN 后不再重试触传输（严格 D08 联动）。 */
			r = ENTELEA_E_STATE;
			break;
		}
		/* RF1 G5：D12 闸逐尝试（每次传输=一次云端请求）——被闸
		 * 即停且不触传输，返回闸结果（闸拒不计风暴）。 */
		gate = entelea_d12_model_allow();
		if (gate != ENTELEA_OK) {
			r = gate;
			break;
		}
		r = transport->chat(transport_user, msgs, n, out, out_max,
				    timeout_ms, &u);
		total_usage.prompt_tokens = usage_add(total_usage.prompt_tokens,
						     u.prompt_tokens);
		total_usage.completion_tokens = usage_add(total_usage.completion_tokens,
							 u.completion_tokens);
		entelea_d12_model_note(r); /* 逐尝试上报（风暴/E_BUSY 冷却） */
		if (r == ENTELEA_E_PERM || entelea_d12_storm_latched()) {
			(void)entelea_dep_mark_down(ENTELEA_DEP_MODEL);
		}
		if (r != ENTELEA_OK) {
			out[0] = '\0';
		}
		if (r != ENTELEA_E_NOT_SENT) {
			break;
		}
	}
	if (usage_out != NULL) {
		*usage_out = total_usage;
	}
	return r;
}

entelea_result_t entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n,
				   char *out, size_t out_max,
				   uint32_t timeout_ms,
				   entelea_model_usage_t *usage_out)
{
	return model_call(msgs, n, out, out_max, timeout_ms, usage_out, false);
}

entelea_result_t entelea_model_probe(const entelea_model_msg_t *msgs, uint8_t n,
				     char *out, size_t out_max,
				     uint32_t timeout_ms,
				     entelea_model_usage_t *usage_out)
{
	return model_call(msgs, n, out, out_max, timeout_ms, usage_out, true);
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
