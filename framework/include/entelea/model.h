/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_MODEL_H
#define ENTELEA_MODEL_H

/* R08 model adaptation layer (B-2; contract framework/services/assistant/
 * contract.md, section "model adaptation layer"). The framework side is the
 * policy layer: dependency linkage (a call is refused if the model or the
 * network is DOWN; D08), bounded retry (only E_NOT_SENT, meaning the request
 * was certainly not sent; the effect of E_IO and E_TIMEOUT may be unknown, so
 * they are not retried automatically), and usage accumulation. Transport is a
 * pluggable ops table (a test mock; the real transport joins the same shape in
 * the real-network or SDK batch; the framework contains no network stack or
 * TLS). Reasoning-model behavior (measured on glm-5.3-flash): responses first
 * consume reasoning tokens. The transport must handle "truncated without
 * content = E_RANGE" and relax the timeout floor. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_MODEL_RETRIES 2U /* retry limit when confirmed not sent */
#define ENTELEA_MODEL_OUT_MIN 64U /* minimum output buffer (suggested) */

typedef struct {
	const char *role; /* "system"/"user"/"assistant" */
	const char *content;
} entelea_model_msg_t;

typedef struct {
	uint32_t prompt_tokens;
	uint32_t completion_tokens; /* includes reasoning tokens (measured) */
} entelea_model_usage_t;

typedef struct {
	/** Single chat completion. Returns E_NOT_SENT only when the request is
	 * certainly not sent. A failure that may have sent it must return
	 * E_IO, E_TIMEOUT, or similar. usage is best-effort (0 = unknown). */
	entelea_result_t (*chat)(void *user, const entelea_model_msg_t *msgs,
				 uint8_t n, char *out, size_t out_max,
				 uint32_t timeout_ms,
				 entelea_model_usage_t *usage);
} entelea_model_transport_t;

/** [boot] Binds the transport (NULL unbinds; ask is refused). */
void
entelea_model_bind(const entelea_model_transport_t *t, void *user);

/** [any] Ready means a transport is bound, the model and network are UP,
 * and the storm latch is not locked. */
bool
entelea_model_ready(void);

/** [thread] Asks a question (policy layer). If unbound or a dependency is
 * DOWN, returns E_STATE without touching the transport. Only E_NOT_SENT is
 * retried, up to ENTELEA_MODEL_RETRIES; E_IO, E_TIMEOUT, and similar are not
 * retried automatically. usage accumulates per call (usage_out is the running
 * total since start; the caller takes the difference for a single call). Each
 * transport attempt passes the D12 gate before it runs and is reported after.
 * If the gate refuses, it stops: returns E_BUSY or E_STATE, does not touch
 * the transport, and does not count toward the storm. */
entelea_result_t
entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n, char *out,
		  size_t out_max, uint32_t timeout_ms,
		  entelea_model_usage_t *usage_out);

/** [thread] Explicit recovery probe: the network must be UP, and the probe
 * still passes the same D12 gate. It may run while the model is not yet UP.
 * One success is only one real observation. The caller reports it to the
 * dependency state machine; one call must not be reported as several
 * successes. */
entelea_result_t
entelea_model_probe(const entelea_model_msg_t *msgs, uint8_t n, char *out,
		    size_t out_max, uint32_t timeout_ms,
		    entelea_model_usage_t *usage_out);

/** [any] Cumulative usage (for observation: budget and ledger surface). */
entelea_model_usage_t
entelea_model_usage_total(void);

#ifdef ENTELEA_TEST
void
entelea_model_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_MODEL_H */
