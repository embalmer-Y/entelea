/* SPDX-License-Identifier: Apache-2.0 */
/*
 * D03 Lua feasibility on the real interpreter (Zephyr only). The single
 * case asserts that every check behaved as the integration contract expects;
 * the measured values are in the "[lua-feas]" lines. Opt-in experiment:
 * build with -DLUA_VM_FEASIBILITY=ON (see docs/lua-feasibility.md).
 */
#include <zephyr/ztest.h>
#include "feasibility.h"

ZTEST(lua_feas, test_contract_holds_on_the_real_interpreter)
{
	uint32_t failed = entelea_lua_feasibility_run();

	/* Each bit of the mask names one check that did not behave as the
	 * contract expects; see feasibility.c. */
	zassert_equal(failed, 0, "failed mask 0x%x", failed);
}

ZTEST_SUITE(lua_feas, NULL, NULL, NULL, NULL, NULL);
