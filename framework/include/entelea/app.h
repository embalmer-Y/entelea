/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_APP_H
#define ENTELEA_APP_H

#include <stdint.h>
#include <entelea/context.h>
#include <entelea/time.h>

/* Recognized payload kinds do not imply an installed or supported runtime. */
typedef enum {
	ENTELEA_RUNTIME_WASM = 1,
	ENTELEA_RUNTIME_LUA = 2
} entelea_runtime_kind_t;

typedef enum {
	ENTELEA_METER_RUNTIME_STEPS = 1,
	ENTELEA_METER_CPU_US = 2
} entelea_meter_unit_t;

typedef struct {
	uint64_t memory_bytes;
	uint64_t execution_limit;
	entelea_meter_unit_t execution_unit;
	entelea_instant_t deadline;
} entelea_app_budget_t;

typedef struct {
	entelea_runtime_kind_t runtime;
	uint32_t payload_version;
	const entelea_principal_t *principal;
	entelea_app_budget_t budget;
} entelea_app_request_t;

typedef enum {
	ENTELEA_EFFECT_NONE = 0,
	ENTELEA_EFFECT_CONFIRMED = 1,
	ENTELEA_EFFECT_UNKNOWN = 2
} entelea_effect_t;

typedef enum {
	ENTELEA_CALL_ACCEPTED = 1,
	ENTELEA_CALL_SUCCEEDED = 2,
	ENTELEA_CALL_FAILED = 3,
	ENTELEA_CALL_CANCELLED = 4
} entelea_call_state_t;

typedef struct {
	uint64_t request_id;
	entelea_call_state_t state;
	entelea_result_t result;
	entelea_effect_t effect;
	entelea_source_t source;
} entelea_call_receipt_t;

#endif
