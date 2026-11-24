/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The C library's malloc family, served from memory pools that a board
 * declares in its devicetree.
 *
 * A pool is a memory node carrying zephyr,memory-attr with the software
 * attribute ATTR_SW_ALLOC_CACHE. This file names no SoC, no address and no
 * linker symbol: what differs between boards is that node and the Kconfig
 * that brings the memory up. See docs/board-osptek-p4c5-resource-budget.md.
 *
 * The C library's entry points cannot be replaced by defining them again:
 * Zephyr's first link pass puts every Zephyr library under --whole-archive.
 * The app therefore redirects the calls with -Wl,--wrap=<name> (see
 * libc_heap.cmake) and the entry points here are __wrap_<name>.
 *
 * Rules:
 *  - Once pools are registered, the malloc family allocates from the pools
 *    only. A full pool yields NULL with errno ENOMEM; nothing falls back to
 *    the small internal RAM behind the caller's back.
 *  - Before that, and on a board that declares no pool, every call goes to
 *    Zephyr's own allocator over the default RAM. Memory obtained that way
 *    is returned there, whatever is registered later.
 *  - Pools are registered at APPLICATION level: Zephyr runs it just before
 *    main(), after every device driver. That is the one readiness statement
 *    that holds on every board, whether the large memory is started by SoC
 *    code or by a driver.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <zephyr/dt-bindings/memory-attr/memory-attr-sw.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/mem_mgmt/mem_attr.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/util.h>

#include "libc_heap.h"

void *
__wrap_malloc(size_t size);
void *
__wrap_realloc(void *ptr, size_t size);
void
__wrap_free(void *ptr);

/* The stock entry points, reachable under their __real_ names. */
void *
__real_malloc(size_t size);
void *
__real_realloc(void *ptr, size_t size);
void *
__real_aligned_alloc(size_t alignment, size_t size);
void
__real_free(void *ptr);

#define POOL_ATTR DT_MEM_SW(ATTR_SW_ALLOC_CACHE)
#define POOL_MAX 8

struct pool {
	struct sys_heap heap;
	const struct mem_attr_region_t *region;
};

static struct pool pools[POOL_MAX];
static size_t pool_count; /* written once at init, before any pool is used */
static K_MUTEX_DEFINE(pool_lock);

/* How a declared region is backed in memory. On hardware the devicetree
 * address is a live pointer; the simulator overrides this one function. */
__weak void *
entelea_libc_heap_region_base(const struct mem_attr_region_t *region)
{
	return (void *) region->dt_addr;
}

static struct pool *
pool_of(const void *ptr)
{
	for (size_t i = 0; i < pool_count; i++) {
		uintptr_t base = (uintptr_t) pools[i].heap.heap;

		if ((uintptr_t) ptr >= base &&
		    (uintptr_t) ptr < base + pools[i].region->dt_size) {
			return &pools[i];
		}
	}
	return NULL;
}

static void *
pool_alloc(size_t alignment, size_t size)
{
	void *block = NULL;

	if (size == 0) {
		return NULL;
	}
	if (pool_count == 0) {
		return alignment ? __real_aligned_alloc(alignment, size) :
				   __real_malloc(size);
	}
	(void) k_mutex_lock(&pool_lock, K_FOREVER);
	for (size_t i = 0; i < pool_count && block == NULL; i++) {
		block = sys_heap_aligned_alloc(&pools[i].heap, alignment, size);
	}
	(void) k_mutex_unlock(&pool_lock);
	if (block == NULL) {
		errno = ENOMEM;
	}
	return block;
}

size_t
entelea_libc_heap_pools(void)
{
	return pool_count;
}

size_t
entelea_libc_heap_bytes(void)
{
	size_t bytes = 0;

	for (size_t i = 0; i < pool_count; i++) {
		bytes += pools[i].region->dt_size;
	}
	return bytes;
}

bool
entelea_libc_heap_owns(const void *ptr)
{
	return pool_of(ptr) != NULL;
}

void *
__wrap_malloc(size_t size)
{
	return pool_alloc(0, size);
}

void *
__wrap_aligned_alloc(size_t alignment, size_t size)
{
	return pool_alloc(alignment, size);
}

void *
__wrap_realloc(void *ptr, size_t size)
{
	struct pool *pool = pool_of(ptr);
	void *block = NULL;

	if (ptr == NULL) {
		return __wrap_malloc(size);
	}
	if (pool == NULL) {
		return __real_realloc(ptr, size);
	}
	if (size == 0) {
		__wrap_free(ptr);
		return NULL;
	}
	(void) k_mutex_lock(&pool_lock, K_FOREVER);
	block = sys_heap_aligned_realloc(&pool->heap, ptr, 0, size);
	(void) k_mutex_unlock(&pool_lock);
	if (block == NULL) {
		errno = ENOMEM;
	}
	return block;
}

void
__wrap_free(void *ptr)
{
	struct pool *pool = pool_of(ptr);

	if (ptr == NULL) {
		return;
	}
	if (pool == NULL) {
		__real_free(ptr);
		return;
	}
	(void) k_mutex_lock(&pool_lock, K_FOREVER);
	sys_heap_free(&pool->heap, ptr);
	(void) k_mutex_unlock(&pool_lock);
}

static int
libc_heap_init(void)
{
	const struct mem_attr_region_t *regions = NULL;
	size_t count = 0;

	if (IS_ENABLED(CONFIG_MEM_ATTR)) {
		count = mem_attr_get_regions(&regions);
	}
	for (size_t i = 0; i < count; i++) {
		const struct mem_attr_region_t *region = &regions[i];
		void *base;

		if (DT_MEM_SW_ATTR_GET(region->dt_attr) != POOL_ATTR) {
			continue;
		}
		base = entelea_libc_heap_region_base(region);
		if (base == NULL || region->dt_size < Z_HEAP_MIN_SIZE) {
			printk("[libc-heap] pool %s unusable (%zu bytes)\n",
			       region->dt_name, region->dt_size);
			continue;
		}
		if (pool_count == POOL_MAX) {
			printk("[libc-heap] pool %s ignored: more than %u "
			       "declared\n",
			       region->dt_name, POOL_MAX);
			continue;
		}
		sys_heap_init(&pools[pool_count].heap, base, region->dt_size);
		pools[pool_count].region = region;
		pool_count++;
		printk("[libc-heap] pool %s: %zu bytes at %p\n",
		       region->dt_name, region->dt_size, base);
	}
	return 0;
}
SYS_INIT(libc_heap_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
