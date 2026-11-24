/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LUA_VM_FEASIBILITY_H
#define ENTELEA_LUA_VM_FEASIBILITY_H

#include <stdint.h>

/* D03 Lua feasibility on the real interpreter (Zephyr only; opt-in
 * experiment, see docs/lua-feasibility.md). Prints one "[lua-feas]" line
 * per check and returns the failed-check mask: 0 when every check behaved
 * as the integration contract expects. */
uint32_t
entelea_lua_feasibility_run(void);

#endif /* ENTELEA_LUA_VM_FEASIBILITY_H */
