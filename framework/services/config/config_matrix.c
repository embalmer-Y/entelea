/* SPDX-License-Identifier: Apache-2.0 */
/* R13 配置装配：机制面自洽检查（合同见 framework/services/config/
 * contract.md §配置矩阵）。产品装配（伴侣）归 owner 硬件批——本面
 * 只声明框架配置组合的自洽约束。 */
#include <string.h>
#include <entelea/config_matrix.h>

bool entelea_config_valid(const entelea_config_t *c)
{
	if (c == NULL) {
		return false;
	}
	/* 依赖自洽：经历记忆/试用晋升需要存储面（持久化承载）。 */
	if ((c->memory || c->delivery) && !c->storage) {
		return false;
	}
	/* 情感/适应依赖经历驱动（R11 合同：适应由经历驱动）。 */
	if (c->affect && !c->memory) {
		return false;
	}
	return true;
}

const char *entelea_config_name(const entelea_config_t *c)
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
