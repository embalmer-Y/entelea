/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_TIME_H
#define ENTELEA_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint64_t ms;
	uint32_t clock_id;
	bool valid;
} entelea_instant_t;

typedef struct {
	uint64_t epoch_ms;
	bool valid;
} entelea_wall_time_t;

typedef struct {
	entelea_result_t (*read_ms)(void *user, uint64_t *out);
	void *user;
	uint32_t clock_id;
	uint64_t last_ms;
	bool sampled;
} entelea_clock_t;

entelea_result_t entelea_clock_init(entelea_clock_t *clock,
	entelea_result_t (*read_ms)(void *, uint64_t *), void *user, uint32_t clock_id);
entelea_result_t entelea_clock_now(entelea_clock_t *clock, entelea_instant_t *out);
entelea_result_t entelea_deadline_after(entelea_instant_t now, uint64_t delay_ms,
	entelea_instant_t *out);
entelea_result_t entelea_deadline_expired(entelea_instant_t now,
	entelea_instant_t deadline, bool *out);

#ifdef __cplusplus
}
#endif
#endif
