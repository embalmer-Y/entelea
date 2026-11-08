/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_CONTEXT_H
#define ENTELEA_CONTEXT_H

#include <stdint.h>

/* Only the trusted host may construct a principal; a source ID is not authority. */
struct entelea_principal;
typedef struct entelea_principal entelea_principal_t;

typedef enum {
	ENTELEA_SOURCE_OBSERVATION = 1,
	ENTELEA_SOURCE_USER = 2,
	ENTELEA_SOURCE_MODEL = 3,
	ENTELEA_SOURCE_DERIVED = 4
} entelea_source_kind_t;

typedef struct {
	uint64_t id;
	entelea_source_kind_t kind;
} entelea_source_t;

#endif
