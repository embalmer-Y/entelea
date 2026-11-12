/* SPDX-License-Identifier: Apache-2.0 */
/* R13 config assembly: self-consistency checks for the mechanism surface
 * (see framework/services/config/contract.md, section "config matrix").
 * Product assembly (companion) belongs to the owner's hardware batch; this
 * surface only declares self-consistency constraints for framework config
 * combinations. */
#include <string.h>
#include <entelea/config_matrix.h>

bool
entelea_config_valid(const entelea_config_t *c)
{
	if (c == NULL) {
		return false;
	}
	/* Memory/delivery (trial promotion) need persistent storage. */
	if ((c->memory || c->delivery) && !c->storage) {
		return false;
	}
	/* Affect/adaptation needs memory (R11 contract: memory-driven). */
	if (c->affect && !c->memory) {
		return false;
	}
	return true;
}

const char *
entelea_config_name(const entelea_config_t *c)
{
	if (c == NULL) {
		return "?";
	}
	if (c->media && c->memory && c->affect) {
		return "full-simulated";
	}
	if (!c->media && !c->memory && !c->affect) {
		return "minimal-assist";
	}
	return "partial";
}
