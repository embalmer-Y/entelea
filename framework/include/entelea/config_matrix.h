/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_CONFIG_MATRIX_H
#define ENTELEA_CONFIG_MATRIX_H

/* R13 configuration assembly contract: the framework configuration is a
 * trimmable combination (media, experience and affect are independently
 * optional; not all are forced on). The product is an MCU desktop companion
 * (enables experience, interaction and applicable media; product acceptance
 * belongs to the owner's hardware batch). This header is the mechanism-level
 * configuration declaration (compile-time composition). */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool storage; /* R03 storage surface */
	bool assistant; /* R08 auxiliary-intelligence loop */
	bool media; /* R09 media interface */
	bool memory; /* R10 experience memory */
	bool affect; /* R11 affect/adaptation */
	bool delivery; /* R12 trial/promotion */
} entelea_config_t;

/* Minimal auxiliary-intelligence configuration: no experience memory, affect
 * or media. A standalone App still runs; this is the real dependency
 * surface. */
#define ENTELEA_CONFIG_MINIMAL_ASSIST            \
	((entelea_config_t) { .storage = true,   \
			      .assistant = true, \
			      .media = false,    \
			      .memory = false,   \
			      .affect = false,   \
			      .delivery = true })

/* Full simulated configuration (mechanism verification surface; not product
 * acceptance). */
#define ENTELEA_CONFIG_FULL_SIMULATED            \
	((entelea_config_t) { .storage = true,   \
			      .assistant = true, \
			      .media = true,     \
			      .memory = true,    \
			      .affect = true,    \
			      .delivery = true })

/** Checks configuration consistency: memory and delivery require storage;
 *  affect requires memory. */
bool
entelea_config_valid(const entelea_config_t *c);

/** Configuration name (for observation). */
const char *
entelea_config_name(const entelea_config_t *c);

#endif
