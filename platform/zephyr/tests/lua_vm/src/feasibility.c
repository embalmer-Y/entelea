/* SPDX-License-Identifier: Apache-2.0 */
/*
 * D03 Lua feasibility: the integration contract exercised on the real, pinned
 * Lua interpreter inside Zephyr (unmodified source, feasibility only; no
 * production dependency is approved). The version is the one pinned in
 * build-support/dependencies.json. The mechanism stubs of
 * lua_feasibility.c are attached to the points they stand in for:
 *   - the memory gate becomes the accounting of a real lua_Allocator,
 *   - the step gate becomes a lua_sethook count hook,
 *   - the host call is a C closure that trusts only the principal the host
 *     bound before the script ran.
 * Besides the checks, every "[lua-feas]" line carries the measured value
 * (memory, steps, microseconds, C-stack depth) for docs/lua-feasibility.md.
 * This is an opt-in experiment, not a Lua runtime; it is never linked into
 * a product image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <entelea/lua_contract.h>
#include <entelea/principal.h>
#include "feasibility.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define FEAS_FAIL_MEM_ACCOUNTING (1u << 0)
#define FEAS_FAIL_MEM_QUOTA (1u << 1)
#define FEAS_FAIL_EXEC_BUDGET (1u << 2)
#define FEAS_FAIL_C_NOT_BOUNDED (1u << 3)
#define FEAS_FAIL_IDENTITY (1u << 4)
#define FEAS_FAIL_ISOLATION (1u << 5)
#define FEAS_FAIL_HOOK_CAN_BE_REMOVED (1u << 6)
#define FEAS_FAIL_SANDBOX_SURFACE (1u << 7)
#define FEAS_FAIL_RECURSION_BOUNDED (1u << 8)
#define FEAS_FAIL_STATE_REUSABLE (1u << 9)
#define FEAS_FAIL_PATTERN_DEPTH (1u << 10)
#define FEAS_FAIL_COROUTINE_BUDGET (1u << 11)
#define FEAS_FAIL_C_NESTING_BOUNDED (1u << 12)
#define FEAS_FAIL_SYNTAX_DEPTH (1u << 13)
#define FEAS_FAIL_C_STACK (1u << 14)
#define FEAS_FAIL_FATAL (1u << 31)

#define FEAS_QUOTA (LUA_VM_QUOTA_KB * 1024u)
#define FEAS_STEPS 100000000u
#define FEAS_NESTED_MAX (LUA_VM_MAXCCALLS + 50)
/* Syntax nesting below and above the interpreter's C-call limit. */
#define FEAS_SYNTAX_OK (LUA_VM_MAXCCALLS - 10)
#define FEAS_SYNTAX_OVER (LUA_VM_MAXCCALLS + 50)

/* The interpreter runs on a dedicated kernel heap. On a board the libc heap
 * is whatever SRAM the image leaves unused: no Kconfig option bounds it, so
 * a quota on top of it would be limited by the image layout rather than by
 * the contract. The quota never exceeds this heap.
 */
#define FEAS_HEAP_BYTES (LUA_VM_HEAP_KB * 1024u)

BUILD_ASSERT(LUA_VM_QUOTA_KB <= LUA_VM_HEAP_KB, "quota larger than the heap");

K_HEAP_DEFINE(feas_heap, FEAS_HEAP_BYTES);

/* ---- C stack depth ----
 *
 * The simulator runs each thread on a host pthread stack, so the kernel's
 * own watermark (k_thread_stack_space_get) would report the thread status
 * kept in the Zephyr stack buffer rather than stack use. This measures
 * directly: the area below the stack pointer is painted before a chunk runs
 * and the deepest overwritten byte is located afterwards. The window is half
 * of the test thread's stack, which is also the limit of what can be
 * reported: a depth equal to the window means "at least".
 */
#define FEAS_STACK_WINDOW (CONFIG_ZTEST_STACK_SIZE / 2)
#define FEAS_STACK_MARK 0x5a

BUILD_ASSERT(CONFIG_ZTEST_STACK_SIZE >= 16384, "C-stack window too small");

/* ---- instance: one interpreter with its own memory and step budgets ---- */

struct feas_inst {
	lua_State *L;
	entelea_lua_mem_budget_t mem;
	entelea_lua_exec_budget_t exec;
	uint32_t hook_refusals;
};

/* Real lua_Allocator over the contract's accounting gate. Lua reports a
 * type tag, not a byte count, in osize when ptr is NULL. */
static void *
feas_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct feas_inst *in = ud;
	size_t old_n = (ptr == NULL && osize < LUA_NUMTYPES) ? 0 : osize;
	void *got;

	if (nsize == 0) {
		(void) entelea_lua_mem_alloc(&in->mem, (uint32_t) old_n, 0);
		k_heap_free(&feas_heap, ptr);
		return NULL;
	}
	if (!entelea_lua_mem_alloc(&in->mem, (uint32_t) old_n,
				   (uint32_t) nsize)) {
		return NULL; /* over quota: the VM takes its OOM path */
	}
	got = k_heap_realloc(&feas_heap, ptr, nsize, K_NO_WAIT);
	if (got == NULL) {
		/* The heap is exhausted although the quota allowed the
		 * request: give the bytes back to keep the accounting truthful. */
		(void) entelea_lua_mem_alloc(&in->mem, (uint32_t) nsize,
					     (uint32_t) old_n);
	}
	return got;
}

/* The hook is inherited by coroutines at creation, together with the extra
 * space, so the instance is found through the running state in both cases. */
static void
feas_hook(lua_State *L, lua_Debug *ar)
{
	struct feas_inst *in = *(struct feas_inst **) lua_getextraspace(L);

	(void) ar;
	if (!entelea_lua_exec_step(&in->exec)) {
		in->hook_refusals++;
		luaL_error(L, "entelea: execution budget exhausted");
	}
}

/* Every standard library is opened except debug, which can replace the hook
 * (owner decision: open as much as possible). The two os functions that act
 * on the whole process are removed by feas_open afterwards. */
static void
feas_open_safe_libs(lua_State *L)
{
	static const luaL_Reg libs[] = {
		{ LUA_GNAME, luaopen_base },
		{ LUA_COLIBNAME, luaopen_coroutine },
		{ LUA_TABLIBNAME, luaopen_table },
		{ LUA_STRLIBNAME, luaopen_string },
		{ LUA_MATHLIBNAME, luaopen_math },
		{ LUA_UTF8LIBNAME, luaopen_utf8 },
		{ LUA_IOLIBNAME, luaopen_io },
		{ LUA_OSLIBNAME, luaopen_os },
		{ LUA_LOADLIBNAME, luaopen_package },
		{ NULL, NULL },
	};
	const luaL_Reg *lib;

	for (lib = libs; lib->func != NULL; lib++) {
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);
	}
}

static void
feas_remove_global(lua_State *L, const char *name)
{
	lua_pushnil(L);
	lua_setglobal(L, name);
}

static void
feas_remove_field(lua_State *L, const char *table, const char *field)
{
	lua_getglobal(L, table);
	lua_pushnil(L);
	lua_setfield(L, -2, field);
	lua_pop(L, 1);
}

static struct feas_inst *
feas_open(uint32_t quota_bytes, uint32_t steps, bool with_debug_lib)
{
	struct feas_inst *in = k_heap_alloc(&feas_heap, sizeof(*in), K_NO_WAIT);

	if (in == NULL) {
		return NULL;
	}
	memset(in, 0, sizeof(*in));
	(void) entelea_lua_mem_init(&in->mem, quota_bytes);
	(void) entelea_lua_exec_init(&in->exec, steps);
	/* The seed only randomizes hash values. */
	in->L = lua_newstate(feas_alloc, in, 0);
	if (in->L == NULL) {
		k_heap_free(&feas_heap, in);
		return NULL;
	}
	*(struct feas_inst **) lua_getextraspace(in->L) = in;
	feas_open_safe_libs(in->L);
	if (with_debug_lib) {
		luaL_requiref(in->L, LUA_DBLIBNAME, luaopen_debug, 1);
		lua_pop(in->L, 1);
	}
	/* The base library is compiled in with its file loaders; a sandbox
	 * removes them instead of patching the Lua sources. They have nothing
	 * to load without a filesystem, and a file, when one exists, must go
	 * through the host's permission gate rather than around it. */
	feas_remove_global(in->L, "dofile");
	feas_remove_global(in->L, "loadfile");
	/* os.exit calls the C exit() and os.execute the C system(): both act
	 * on the whole firmware, not on the script. */
	feas_remove_field(in->L, LUA_OSLIBNAME, "exit");
	feas_remove_field(in->L, LUA_OSLIBNAME, "execute");
	lua_sethook(in->L, feas_hook, LUA_MASKCOUNT, 1);
	return in;
}

static void
feas_close(struct feas_inst *in)
{
	lua_close(in->L);
	k_heap_free(&feas_heap, in);
}

/* Runs a text chunk under a fresh step budget; returns the lua_pcall
 * status (the error object, if any, is left on the stack). */
static int
feas_run(struct feas_inst *in, const char *chunk)
{
	(void) entelea_lua_exec_init(&in->exec, in->exec.step_budget);

	if (luaL_loadbufferx(in->L, chunk, strlen(chunk), "chunk", "t") !=
	    LUA_OK) {
		return LUA_ERRSYNTAX;
	}
	return lua_pcall(in->L, 0, 1, 0);
}

/* The simulator advances its clock only while the CPU idles, so there the
 * host's monotonic clock is read instead. Its declaration is written here:
 * the image sees Zephyr's <time.h>, which carries the simulated clock, while
 * the host library itself is reached only through dynamic linking. */
#if defined(CONFIG_BOARD_NATIVE_SIM)
struct feas_timespec {
	long tv_sec;
	long tv_nsec;
};
extern int
clock_gettime(int clk_id, struct feas_timespec *tp);
#define FEAS_CLOCK_MONOTONIC 1
#endif

static int64_t
feas_now_us(void)
{
#if defined(CONFIG_BOARD_NATIVE_SIM)
	struct feas_timespec ts;

	(void) clock_gettime(FEAS_CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
	return k_uptime_get() * 1000;
#endif
}

/* ---- the host call: identity comes from the closure, never from Lua ---- */

static int
feas_probe(lua_State *L)
{
	const entelea_principal_t *ctx = lua_touserdata(L, lua_upvalueindex(1));
	entelea_result_t r = entelea_perm_check(*ctx, ENTELEA_PERM_GPIO,
						ENTELEA_PERM_READ, 0);

	/* Script arguments take no part in the decision. */
	lua_pushboolean(L, r == ENTELEA_OK);
	return 1;
}

/* A host function that does its work outside the Lua VM. */
static int
feas_busy(lua_State *L)
{
	lua_Integer n = luaL_checkinteger(L, 1);
	volatile lua_Integer acc = 0;

	for (lua_Integer i = 0; i < n; i++) {
		acc += i;
	}
	lua_pushinteger(L, acc);
	return 1;
}

static bool
feas_top_is(lua_State *L, bool expected)
{
	return lua_toboolean(L, -1) == expected;
}

/* ---- C stack measurement ---- */

static uintptr_t __attribute__((noinline))
feas_sp(void)
{
	const volatile char probe = 0;

	return (uintptr_t) &probe;
}

/* Paints the window below the current frame and returns its top. */
static uintptr_t __attribute__((noinline))
feas_paint(void)
{
	volatile uint8_t *top = (volatile uint8_t *) feas_sp();
	size_t i;

	for (i = 1; i <= FEAS_STACK_WINDOW; i++) {
		top[-i] = FEAS_STACK_MARK;
	}
	return (uintptr_t) top;
}

static size_t __attribute__((noinline))
feas_depth(uintptr_t top)
{
	const volatile uint8_t *low =
		(const volatile uint8_t *) top - FEAS_STACK_WINDOW;
	size_t i;

	for (i = 0; i < FEAS_STACK_WINDOW; i++) {
		if (low[i] != FEAS_STACK_MARK) {
			return FEAS_STACK_WINDOW - i;
		}
	}
	return FEAS_STACK_WINDOW;
}

/* Runs a chunk and returns how deep the C stack reached below this frame. */
static size_t
feas_run_measured(struct feas_inst *in, const char *chunk, int *status)
{
	uintptr_t top = feas_paint();
	size_t depth;

	*status = feas_run(in, chunk);
	/* Nothing may run in this thread between the paint and this scan. */
	depth = feas_depth(top);
	return depth;
}

/* "return ((((1))))" with n nested parentheses. */
static void
feas_build_nested(char *buf, size_t cap, unsigned n)
{
	size_t len = strlen("return ");

	__ASSERT(len + 2 * (size_t) n + 2 <= cap, "nested chunk too long");
	memcpy(buf, "return ", len);
	memset(buf + len, '(', n);
	buf[len + n] = '1';
	memset(buf + len + n + 1, ')', n);
	buf[len + 2 * (size_t) n + 1] = '\0';
}

/* string.find over 'items' optional items that all match: every item
 * doubles the backtracking while the recursion stays below its limit. */
static int
feas_backtrack(struct feas_inst *in, unsigned items, int64_t *elapsed_us,
	       size_t *depth)
{
	char chunk[96];
	int64_t t0 = feas_now_us();
	int status;

	snprintf(chunk, sizeof(chunk),
		 "return string.find(string.rep('a', %u), "
		 "string.rep('a?', %u) .. 'b')",
		 items, items);
	*depth = feas_run_measured(in, chunk, &status);
	*elapsed_us = feas_now_us() - t0;
	return status;
}

uint32_t
entelea_lua_feasibility_run(void)
{
	static const char typical[] =
		"local t = {} for i = 1, 1000 do t[i] = i * i end "
		"local s = 0 for i = 1, 1000 do s = s + t[i] end return s";
	static const char loop[] =
		"local n = 0 while n < 20000000 do n = n + 1 end return n";
	/* 0 means no hook. */
	static const int counts[] = { 0, 1, 100, 10000 };
	uint32_t failed = 0;
	struct feas_inst *in;
	struct feas_inst *other;
	entelea_principal_t principal = { 0 };
	entelea_perm_table_t table;
	char nested[FEAS_NESTED_MAX * 2 + 16];
	size_t deepest = 0;
	size_t depth;
	uint32_t denied_before;
	uint32_t used_before;
	int64_t t0;
	int64_t t1;
	int64_t elapsed;
	int status;

	printk("[lua-feas] Lua %s, LUAI_MAXCCALLS=%d\n", LUA_VERSION,
	       LUA_VM_MAXCCALLS);
	printk("[lua-feas] heap: dedicated kernel heap of %u bytes, quota "
	       "under "
	       "test %d KB\n",
	       (uint32_t) FEAS_HEAP_BYTES, LUA_VM_QUOTA_KB);

	/* ---- 1. memory budget: accounting, baseline, quota ---- */
	in = feas_open(FEAS_QUOTA, FEAS_STEPS, false);
	if (in == NULL) {
		printk("[lua-feas] FATAL cannot create a Lua state\n");
		return FEAS_FAIL_FATAL;
	}
	printk("[lua-feas] mem: state+libs peak=%u used=%u denied=%u\n",
	       in->mem.peak_bytes, in->mem.used_bytes, in->mem.denied);
	if (in->mem.used_bytes == 0 ||
	    in->mem.peak_bytes < in->mem.used_bytes) {
		failed |= FEAS_FAIL_MEM_ACCOUNTING;
	}
	depth = feas_run_measured(in, "return", &status);
	printk("[lua-feas] stack: empty chunk status=%d c_stack=%zu\n", status,
	       depth);
	if (status != LUA_OK) {
		failed |= FEAS_FAIL_MEM_ACCOUNTING;
	}
	lua_pop(in->L, 1);
	deepest = MAX(deepest, depth);
	depth = feas_run_measured(in, typical, &status);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] mem: typical script status=%d result=%lld peak=%u "
	       "c_stack=%zu\n",
	       status, (long long) lua_tointeger(in->L, -1), in->mem.peak_bytes,
	       depth);
	if (status != LUA_OK || lua_tointeger(in->L, -1) != 333833500LL) {
		failed |= FEAS_FAIL_MEM_ACCOUNTING;
	}
	lua_pop(in->L, 1);
	denied_before = in->mem.denied;
	status = feas_run(in, "local a = {} for i = 1, 20000 do "
			      "a[i] = string.rep('x', 100) end return #a");
	printk("[lua-feas] mem: 2 MB of strings under a %u KB quota status=%d "
	       "(LUA_ERRMEM=%d) denials=%u used=%u\n",
	       FEAS_QUOTA / 1024u, status, LUA_ERRMEM,
	       in->mem.denied - denied_before, in->mem.used_bytes);
	if (status != LUA_ERRMEM || in->mem.denied == denied_before ||
	    in->mem.used_bytes > in->mem.quota_bytes) {
		failed |= FEAS_FAIL_MEM_QUOTA;
	}
	lua_pop(in->L, 1);

	/* ---- 2. execution budget: bounded stop and hook cost ---- */
	in->exec.step_budget = 100000u;
	depth = feas_run_measured(in, "while true do end", &status);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] exec: endless loop budget=100000 status=%d steps=%u "
	       "refusals=%u c_stack=%zu msg=%s\n",
	       status, in->exec.steps_used, in->hook_refusals, depth,
	       lua_tostring(in->L, -1) ? lua_tostring(in->L, -1) : "?");
	if (status != LUA_ERRRUN ||
	    in->exec.steps_used > in->exec.step_budget ||
	    in->hook_refusals != 1) {
		failed |= FEAS_FAIL_EXEC_BUDGET;
	}
	lua_pop(in->L, 1);
	/* Cost of the step gate: the hook fires every 'count' VM instructions,
	 * and a stop is at most that many instructions late. */
	in->exec.step_budget = FEAS_STEPS;
	for (size_t i = 0; i < ARRAY_SIZE(counts); i++) {
		if (counts[i] == 0) {
			lua_sethook(in->L, NULL, 0, 0);
		} else {
			lua_sethook(in->L, feas_hook, LUA_MASKCOUNT, counts[i]);
		}
		t0 = feas_now_us();
		status = feas_run(in, loop);
		t1 = feas_now_us();
		if (counts[i] == 0) {
			printk("[lua-feas] exec: 2e7 iterations without the "
			       "hook "
			       "status=%d elapsed_us=%lld\n",
			       status, (long long) (t1 - t0));
		} else {
			printk("[lua-feas] exec: 2e7 iterations, hook every %d "
			       "instructions status=%d elapsed_us=%lld\n",
			       counts[i], status, (long long) (t1 - t0));
		}
		lua_pop(in->L, 1);
		if (status != LUA_OK) {
			failed |= FEAS_FAIL_EXEC_BUDGET;
		}
	}
	lua_sethook(in->L, feas_hook, LUA_MASKCOUNT, 1);

	/* ---- 3. what the count hook does not bound: work done in C ---- */
	lua_pushcfunction(in->L, feas_busy);
	lua_setglobal(in->L, "busy");
	in->exec.step_budget = 1000u;
	t0 = feas_now_us();
	status = feas_run(in, "return busy(100000000)");
	t1 = feas_now_us();
	printk("[lua-feas] exec: host busy loop budget=1000 status=%d steps=%u "
	       "elapsed_us=%lld (runs to completion)\n",
	       status, in->exec.steps_used, (long long) (t1 - t0));
	lua_pop(in->L, 1);
	if (status != LUA_OK || in->exec.steps_used > in->exec.step_budget) {
		failed |= FEAS_FAIL_C_NOT_BOUNDED;
	}
	in->exec.step_budget = 1000u;
	status = feas_backtrack(in, 22u, &elapsed, &depth);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] exec: 22-item backtracking string.find budget=1000 "
	       "status=%d elapsed_us=%lld c_stack=%zu\n",
	       status, (long long) elapsed, depth);
	lua_pop(in->L, 1);
	in->exec.step_budget = 1000u;
	status = feas_backtrack(in, 24u, &elapsed, &depth);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] exec: 24-item backtracking string.find budget=1000 "
	       "status=%d elapsed_us=%lld c_stack=%zu\n",
	       status, (long long) elapsed, depth);
	lua_pop(in->L, 1);
	if (status != LUA_OK) {
		failed |= FEAS_FAIL_C_NOT_BOUNDED;
	}

	/* The pattern matcher recurses in C; stock Lua limits the depth. */
	in->exec.step_budget = 1000000u;
	depth = feas_run_measured(in,
				  "return string.find(string.rep('a', 300), "
				  "string.rep('a?', 300))",
				  &status);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] exec: 300-item pattern recursion status=%d "
	       "c_stack=%zu msg=%s\n",
	       status, depth,
	       lua_tostring(in->L, -1) ? lua_tostring(in->L, -1) : "?");
	if (status != LUA_ERRRUN) {
		failed |= FEAS_FAIL_PATTERN_DEPTH;
	}
	lua_pop(in->L, 1);

	/* A coroutine is its own Lua state; it inherits the hook when it is
	 * created. */
	in->exec.step_budget = 100000u;
	status = feas_run(in,
			  "local co = coroutine.create(function() local n = 0 "
			  "while n < 30000000 do n = n + 1 end return n end) "
			  "local ok, n = coroutine.resume(co) return ok and n");
	printk("[lua-feas] exec: endless loop inside a coroutine budget=100000 "
	       "status=%d steps=%u\n",
	       status, in->exec.steps_used);
	/* The stop is total: once the budget is spent, the refusals also end
	 * the chunk that resumed the coroutine. */
	if (status != LUA_ERRRUN ||
	    in->exec.steps_used > in->exec.step_budget) {
		failed |= FEAS_FAIL_COROUTINE_BUDGET;
	}
	lua_pop(in->L, 1);

	/* Lua called back from C called from Lua: every lap is one more
	 * nested C call on the C stack. Stock Lua limits the nesting. */
	in->exec.step_budget = 1000000u;
	depth = feas_run_measured(
		in,
		"local function g(n) if n == 0 then return "
		"'x' end return string.gsub('a', 'a', function() "
		"return g(n - 1) end) end return g(1000)",
		&status);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] exec: 1000 nested C calls through string.gsub "
	       "status=%d c_stack=%zu msg=%s\n",
	       status, depth,
	       lua_tostring(in->L, -1) ? lua_tostring(in->L, -1) : "?");
	if (status != LUA_ERRRUN) {
		failed |= FEAS_FAIL_C_NESTING_BOUNDED;
	}
	lua_pop(in->L, 1);

	/* ---- 4. sandbox surface ---- */
	in->exec.step_budget = 1000000u;
	status = feas_run(in,
			  "return debug == nil and dofile == nil and "
			  "loadfile == nil and os.exit == nil and "
			  "os.execute == nil and io ~= nil and os ~= nil and "
			  "package ~= nil");
	printk("[lua-feas] sandbox: debug and the file loaders closed, os.exit "
	       "and os.execute removed, io/os/package open status=%d "
	       "result=%d\n",
	       status, (int) lua_toboolean(in->L, -1));
	if (status != LUA_OK || !feas_top_is(in->L, true)) {
		failed |= FEAS_FAIL_SANDBOX_SURFACE;
	}
	lua_pop(in->L, 1);
#if !defined(CONFIG_BOARD_NATIVE_SIM)
	/* With the fail-closed stubs a board refuses to open any file. The
	 * simulator has a real host filesystem behind io and os, so this is
	 * asserted on boards only. */
	in->exec.step_budget = 1000000u;
	status = feas_run(in, "local f, err = io.open('entelea-probe', 'r') "
			      "return f == nil and type(err) == 'string'");
	printk("[lua-feas] sandbox: io.open refused status=%d result=%d\n",
	       status, (int) lua_toboolean(in->L, -1));
	if (status != LUA_OK || !feas_top_is(in->L, true)) {
		failed |= FEAS_FAIL_SANDBOX_SURFACE;
	}
	lua_pop(in->L, 1);
#endif

	/* ---- 5. limits that end with an error, and the state survives ---- */
	feas_build_nested(nested, sizeof(nested), FEAS_SYNTAX_OK);
	depth = feas_run_measured(in, nested, &status);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] stack: %d nested parentheses status=%d "
	       "c_stack=%zu\n",
	       FEAS_SYNTAX_OK, status, depth);
	if (status != LUA_OK) {
		failed |= FEAS_FAIL_SYNTAX_DEPTH;
	}
	lua_pop(in->L, 1);
	feas_build_nested(nested, sizeof(nested), FEAS_SYNTAX_OVER);
	depth = feas_run_measured(in, nested, &status);
	deepest = MAX(deepest, depth);
	printk("[lua-feas] stack: %d nested parentheses status=%d c_stack=%zu "
	       "msg=%s (syntax-level limit)\n",
	       FEAS_SYNTAX_OVER, status, depth,
	       lua_tostring(in->L, -1) ? lua_tostring(in->L, -1) : "?");
	if (status != LUA_ERRSYNTAX) {
		failed |= FEAS_FAIL_SYNTAX_DEPTH;
	}
	lua_pop(in->L, 1);
	in->exec.step_budget = 1000u;
	(void) feas_run(in, "while true do end");
	lua_pop(in->L, 1);
	in->exec.step_budget = 1000000u;
	status = feas_run(in,
			  "local function f() return 1 + f() end return f()");
	printk("[lua-feas] exec: runaway recursion status=%d msg=%s\n", status,
	       lua_tostring(in->L, -1) ? lua_tostring(in->L, -1) : "?");
	/* Lua-to-Lua recursion uses no C stack: the quota or the Lua stack
	 * limit ends it, either way with an error. */
	if (status != LUA_ERRRUN && status != LUA_ERRMEM) {
		failed |= FEAS_FAIL_RECURSION_BOUNDED;
	}
	lua_pop(in->L, 1);
	status = feas_run(in, "return 6 * 7");
	printk("[lua-feas] reuse after aborts status=%d result=%lld\n", status,
	       (long long) lua_tointeger(in->L, -1));
	if (status != LUA_OK || lua_tointeger(in->L, -1) != 42) {
		failed |= FEAS_FAIL_STATE_REUSABLE;
	}
	feas_close(in);

	/* ---- 6. host call and trusted identity ---- */
	in = feas_open(FEAS_QUOTA, 1000000u, false);
	if (in == NULL) {
		return failed | FEAS_FAIL_FATAL;
	}
	entelea_perm_table_init(&table);
	(void) entelea_perm_parse("gpio:read:0", &table);
	principal._handle = 0; /* unbound until the host binds it */
	lua_pushlightuserdata(in->L, &principal);
	lua_pushcclosure(in->L, feas_probe, 1);
	lua_setglobal(in->L, "probe");
	status = feas_run(in, "return probe(1, 1, 42) == false and "
			      "probe() == false");
	printk("[lua-feas] identity: unbound principal status=%d result=%d "
	       "(script arguments take no part)\n",
	       status, (int) lua_toboolean(in->L, -1));
	if (status != LUA_OK || !feas_top_is(in->L, true)) {
		failed |= FEAS_FAIL_IDENTITY;
	}
	if (entelea_principal_bind(&principal, 42, &table) != ENTELEA_OK) {
		failed |= FEAS_FAIL_IDENTITY;
	}
	status = feas_run(in, "return probe(0) == true");
	printk("[lua-feas] identity: bound principal status=%d result=%d\n",
	       status, (int) lua_toboolean(in->L, -1));
	if (status != LUA_OK || !feas_top_is(in->L, true)) {
		failed |= FEAS_FAIL_IDENTITY;
	}
	entelea_principal_unbind(&principal);
	feas_close(in);

	/* ---- 7. data isolation: two instances, two budgets ---- */
	in = feas_open(64u * 1024u, 1000000u, false);
	other = feas_open(64u * 1024u, 1000000u, false);
	if (in == NULL || other == NULL) {
		return failed | FEAS_FAIL_FATAL;
	}
	(void) feas_run(in, "x = {'secret'}; return x[1]");
	status = feas_run(other, "return x");
	printk("[lua-feas] isolation: first sees '%s', second sees a %s\n",
	       lua_tostring(in->L, -1) ? lua_tostring(in->L, -1) : "?",
	       lua_typename(other->L, lua_type(other->L, -1)));
	if (lua_type(other->L, -1) != LUA_TNIL) {
		failed |= FEAS_FAIL_ISOLATION;
	}
	lua_pop(in->L, 1);
	lua_pop(other->L, 1);
	used_before = other->mem.used_bytes;
	(void) feas_run(in, "local a = {} for i = 1, 20000 do "
			    "a[i] = string.rep('x', 100) end return #a");
	status = feas_run(other, "return #string.rep('y', 1000)");
	printk("[lua-feas] isolation: first denied=%u, second still allocates "
	       "status=%d result=%lld\n",
	       in->mem.denied, status, (long long) lua_tointeger(other->L, -1));
	if (in->mem.denied == 0 || status != LUA_OK ||
	    lua_tointeger(other->L, -1) != 1000 ||
	    other->mem.used_bytes <= used_before) {
		failed |= FEAS_FAIL_ISOLATION;
	}
	feas_close(in);
	feas_close(other);

	/* ---- 8. hazard shown: the debug library removes the budget ---- */
	in = feas_open(FEAS_QUOTA, 100000u, true);
	if (in == NULL) {
		return failed | FEAS_FAIL_FATAL;
	}
	status = feas_run(in, "debug.sethook() local n = 0 while n < 50000000 "
			      "do n = n + 1 end return n");
	printk("[lua-feas] hazard: debug.sethook() removes the budget "
	       "status=%d steps=%u result=%lld (so a sandbox must not open "
	       "debug)\n",
	       status, in->exec.steps_used,
	       (long long) lua_tointeger(in->L, -1));
	if (status != LUA_OK || in->exec.steps_used >= in->exec.step_budget) {
		failed |= FEAS_FAIL_HOOK_CAN_BE_REMOVED;
	}
	feas_close(in);

	printk("[lua-feas] stack: deepest C-stack use %s%zu bytes of a "
	       "%zu-byte "
	       "window\n",
	       deepest >= FEAS_STACK_WINDOW ? "at least " : "", deepest,
	       (size_t) FEAS_STACK_WINDOW);
	/* A saturated window is a lower bound, not a failed measurement:
	 * CONFIG_ZTEST_STACK_SIZE is doubled to resolve it. */
	if (deepest == 0) {
		failed |= FEAS_FAIL_C_STACK;
	}
	printk("[lua-feas] failed mask=0x%x\n", failed);
	return failed;
}
