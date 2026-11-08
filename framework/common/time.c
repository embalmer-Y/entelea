/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include <entelea/time.h>

entelea_result_t entelea_clock_init(entelea_clock_t *clock,
	entelea_result_t (*read_ms)(void *, uint64_t *), void *user, uint32_t clock_id)
{
	if (clock == NULL || read_ms == NULL || clock_id == 0) {
		return ENTELEA_E_PARAM;
	}
	*clock = (entelea_clock_t){.read_ms = read_ms, .user = user, .clock_id = clock_id};
	return ENTELEA_OK;
}

entelea_result_t entelea_clock_now(entelea_clock_t *clock, entelea_instant_t *out)
{
	if (clock == NULL || out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (clock->read_ms == NULL || clock->clock_id == 0) {
		return ENTELEA_E_UNINIT;
	}
	uint64_t ms = 0;
	entelea_result_t result = clock->read_ms(clock->user, &ms);

	if (result != ENTELEA_OK) {
		return result;
	}
	if (clock->sampled && ms < clock->last_ms) {
		return ENTELEA_E_STATE;
	}
	clock->last_ms = ms;
	clock->sampled = true;
	*out = (entelea_instant_t){.ms = ms, .clock_id = clock->clock_id, .valid = true};
	return ENTELEA_OK;
}

entelea_result_t entelea_deadline_after(entelea_instant_t now, uint64_t delay_ms,
	entelea_instant_t *out)
{
	if (out == NULL || !now.valid || now.clock_id == 0) {
		return ENTELEA_E_PARAM;
	}
	if (delay_ms > UINT64_MAX - now.ms) {
		return ENTELEA_E_RANGE;
	}
	*out = now;
	out->ms += delay_ms;
	return ENTELEA_OK;
}

entelea_result_t entelea_deadline_expired(entelea_instant_t now,
	entelea_instant_t deadline, bool *out)
{
	if (out == NULL || !now.valid || !deadline.valid || now.clock_id == 0 ||
	    now.clock_id != deadline.clock_id) {
		return ENTELEA_E_PARAM;
	}
	*out = now.ms >= deadline.ms;
	return ENTELEA_OK;
}
