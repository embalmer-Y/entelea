/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HTTPS_TIME_H
#define ENTELEA_HTTPS_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <entelea/result.h>

typedef bool (*https_time_reader_t)(uint64_t *now_ms);

typedef struct {
	uint64_t start_ms;
	uint64_t last_ms;
	uint32_t timeout_ms;
} https_timer_t;

static entelea_result_t
https_timer_start(https_timer_t *timer, https_time_reader_t read_ms,
		  uint32_t timeout_ms)
{
	uint64_t now;

	if (!read_ms(&now)) {
		return ENTELEA_E_STATE;
	}
	timer->start_ms = now;
	timer->last_ms = now;
	timer->timeout_ms = timeout_ms;
	return timeout_ms == 0 ? ENTELEA_E_TIMEOUT : ENTELEA_OK;
}

static entelea_result_t
https_timer_check(https_timer_t *timer, https_time_reader_t read_ms)
{
	uint64_t now;

	if (!read_ms(&now) || now < timer->last_ms) {
		return ENTELEA_E_STATE;
	}
	timer->last_ms = now;
	return now - timer->start_ms >= timer->timeout_ms ? ENTELEA_E_TIMEOUT :
							    ENTELEA_OK;
}

#endif /* ENTELEA_HTTPS_TIME_H */
