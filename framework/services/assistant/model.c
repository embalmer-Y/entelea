/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R08 model adapter implementation (B-2; contract
 * assistant/contract.md, "model adapter layer"). Policy surface:
 * dependency linkage (model + network), bounded retry while a send is
 * confirmed unsent, and usage accumulation. The transport surface is
 * owned entirely by pluggable ops (mock tests close the loop; the real
 * transport comes with real-network batches, since no network stack or
 * TLS enters the framework). No timers: retries are immediate, bounded
 * resends (count capped by ENTELEA_MODEL_RETRIES to keep cost
 * controlled; the backoff policy is calibrated with real-device batches).
 * Every transport attempt passes the D12 gate and is reported per attempt
 * (RF1 G5: retries do not bypass frequency or storm limits).
 */
#include <string.h>
#include <entelea/d12.h>
#include <entelea/model.h>
#include <entelea/netmon.h>

static const entelea_model_transport_t *transport;
static void *transport_user;
static entelea_model_usage_t total_usage;

static uint32_t
usage_add(uint32_t total, uint32_t value)
{
	return value > UINT32_MAX - total ? UINT32_MAX : total + value;
}

void
entelea_model_bind(const entelea_model_transport_t *t, void *user)
{
	transport = t;
	transport_user = user;
}

bool
entelea_model_ready(void)
{
	return transport != NULL && transport->chat != NULL &&
	       entelea_dep_state(ENTELEA_DEP_MODEL) == ENTELEA_DEP_UP &&
	       entelea_dep_state(ENTELEA_DEP_NET) == ENTELEA_DEP_UP &&
	       !entelea_d12_storm_latched();
}

static entelea_result_t
model_call(const entelea_model_msg_t *msgs, uint8_t n, char *out,
	   size_t out_max, uint32_t timeout_ms,
	   entelea_model_usage_t *usage_out, bool probe)
{
	if (msgs == NULL || n == 0 || out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (out_max < ENTELEA_MODEL_OUT_MIN) {
		return ENTELEA_E_RANGE; /* Output too small: will truncate */
	}
	out[0] = '\0';
	if (transport == NULL || transport->chat == NULL) {
		return ENTELEA_E_UNINIT;
	}
	if (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP ||
	    (!probe && !entelea_model_ready())) {
		return ENTELEA_E_STATE; /* Deps down: no transport (D08) */
	}
	entelea_result_t r = ENTELEA_E_IO;

	for (uint32_t attempt = 0; attempt <= ENTELEA_MODEL_RETRIES;
	     attempt++) {
		entelea_model_usage_t u = { 0 };
		entelea_result_t gate;

		if (attempt > 0 &&
		    (entelea_dep_state(ENTELEA_DEP_NET) != ENTELEA_DEP_UP ||
		     (!probe && !entelea_model_ready()))) {
			/* review P3-5: re-check dependencies on each attempt;
			 * once the transport callback reports one DOWN, stop
			 * retrying (strict D08 linkage). */
			r = ENTELEA_E_STATE;
			break;
		}
		/* RF1 G5: D12 gate per attempt (each transport is one cloud
		 * request). If the gate blocks, stop without a transport call
		 * and return the gate result; refusals are not storms. */
		gate = entelea_d12_model_allow();
		if (gate != ENTELEA_OK) {
			r = gate;
			break;
		}
		r = transport->chat(transport_user, msgs, n, out, out_max,
				    timeout_ms, &u);
		total_usage.prompt_tokens =
			usage_add(total_usage.prompt_tokens, u.prompt_tokens);
		total_usage.completion_tokens = usage_add(
			total_usage.completion_tokens, u.completion_tokens);
		entelea_d12_model_note(r); /* Report per try (storm, E_BUSY) */
		if (r == ENTELEA_E_PERM || entelea_d12_storm_latched()) {
			(void) entelea_dep_mark_down(ENTELEA_DEP_MODEL);
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

entelea_result_t
entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n, char *out,
		  size_t out_max, uint32_t timeout_ms,
		  entelea_model_usage_t *usage_out)
{
	return model_call(msgs, n, out, out_max, timeout_ms, usage_out, false);
}

entelea_result_t
entelea_model_probe(const entelea_model_msg_t *msgs, uint8_t n, char *out,
		    size_t out_max, uint32_t timeout_ms,
		    entelea_model_usage_t *usage_out)
{
	return model_call(msgs, n, out, out_max, timeout_ms, usage_out, true);
}

entelea_model_usage_t
entelea_model_usage_total(void)
{
	return total_usage;
}

#ifdef ENTELEA_TEST
void
entelea_model_test_reset(void)
{
	transport = NULL;
	transport_user = NULL;
	memset(&total_usage, 0, sizeof(total_usage));
}
#endif
