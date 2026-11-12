/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <entelea/affect.h>

#ifdef ENTELEA_TEST
#error "Cold-start verification must use the production affect configuration"
#endif

ZTEST(framework_affect_cold, test_initial_levels_are_midpoint)
{
	entelea_affect_state_t state;

	entelea_affect_get(&state);
	for (int i = 0; i < ENTELEA_AFFECT_LEVELS; i++) {
		zassert_equal(state.level[i], 50, "冷启动初值应为中位");
	}
}

ZTEST_SUITE(framework_affect_cold, NULL, NULL, NULL, NULL, NULL);
