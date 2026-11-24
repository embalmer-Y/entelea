/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libc 分配器测试（platform/zephyr/libc_heap.c）：malloc 族由设备树声明的
 * 内存池供数。真机上池子是板级设备树声明的整块 PSRAM，仿真器上是
 * boards/native_sim.overlay 声明的两块小区域。断言一律以登记的池子大小
 * 为准，同一个测试在两边运行。
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "libc_heap.h"

/* 真机：池子必须是板级设备树声明的整块 PSRAM。 */
#if DT_NODE_EXISTS(DT_NODELABEL(psram_heap))
#define BOARD_POOL_BYTES DT_REG_SIZE(DT_NODELABEL(psram_heap))
#else
#define BOARD_POOL_BYTES 0
#endif

#define BLOCK_SIZE 65536u
#define BLOCK_MAX 1024u /* 64 MiB，够放下整块 PSRAM */
#define WORKERS 3
#define ROUNDS 300

/* 每块分配的头部与对齐开销远小于 64 字节，另有池子末尾的标记与对齐。 */
#define OVERHEAD_PER_BLOCK 64u
#define POOL_FIXED_OVERHEAD 4096u

static uint8_t *blocks[BLOCK_MAX];
static uint8_t *early_block;

/* 一次分配只能落在一个池子里。只有一个池子时几乎可以拿下整块；有多个时
 * 取“平均每个池子的大小”，保证放得进其中任何一个。 */
static size_t
largest_single(void)
{
	size_t bytes = entelea_libc_heap_bytes();

	if (entelea_libc_heap_pools() == 1) {
		return bytes - 8192;
	}
	return bytes / entelea_libc_heap_pools() - 8192;
}

static void
fill(uint8_t *mem, size_t size, uint8_t value)
{
	memset(mem, value, size);
}

static bool
intact(const uint8_t *mem, size_t size, uint8_t value)
{
	for (size_t i = 0; i < size; i++) {
		if (mem[i] != value) {
			return false;
		}
	}
	return true;
}

/* 池子登记（APPLICATION 级）之前的一次分配：它来自原生分配器，后面的用例
 * 检查它被正确归还。排在原生堆初始化（POST_KERNEL 35）之后。 */
static int
allocate_before_pools(void)
{
	early_block = malloc(64);
	return 0;
}
SYS_INIT(allocate_before_pools, POST_KERNEL, 36);

ZTEST(libc_heap, test_pool_is_the_declared_memory)
{
	zassert_true(entelea_libc_heap_pools() >= 1, "没有登记任何池子");
	zassert_true(entelea_libc_heap_bytes() >= 256u * 1024u,
		     "池子太小，跑不了本测试：%zu 字节",
		     entelea_libc_heap_bytes());
	if (BOARD_POOL_BYTES != 0) {
		zassert_equal(entelea_libc_heap_pools(), 1,
			      "本板只声明一个池子");
		zassert_equal(entelea_libc_heap_bytes(), BOARD_POOL_BYTES,
			      "池子就是板级声明的整块");
	}
}

ZTEST(libc_heap, test_whole_pool_allocatable_writable_and_returned)
{
	const size_t bytes = entelea_libc_heap_bytes();
	const size_t whole_size = largest_single();
	size_t count = 0;
	size_t got = 0;
	uint8_t *whole;

	for (; count < BLOCK_MAX; count++) {
		uint8_t *block = malloc(BLOCK_SIZE);

		if (block == NULL) {
			break;
		}
		zassert_true(entelea_libc_heap_owns(block), "分配来自池子");
		fill(block, BLOCK_SIZE, (uint8_t) (0x5a ^ count));
		blocks[count] = block;
		got += BLOCK_SIZE;
	}
	zassert_true(count > 0, "第一块都分不出来");
	zassert_equal(errno, ENOMEM, "池子分满后报 ENOMEM");
	/* 每个池子末尾至多剩不到一块。 */
	zassert_true(bytes - got < entelea_libc_heap_pools() * BLOCK_SIZE +
					   count * OVERHEAD_PER_BLOCK +
					   POOL_FIXED_OVERHEAD,
		     "池子几乎全部交出：交出 %zu / %zu 字节", got, bytes);
	for (size_t i = 0; i < count; i++) {
		zassert_true(
			intact(blocks[i], BLOCK_SIZE, (uint8_t) (0x5a ^ i)),
			"第 %zu 块的内容被改动", i);
	}
	for (size_t i = 0; i < count; i++) {
		free(blocks[i]);
		blocks[i] = NULL;
	}
	/* 全部归还后一次拿下几乎整块，首尾都要能写能读。 */
	whole = malloc(whole_size);
	zassert_not_null(whole, "归还后应能一次拿下几乎整块");
	fill(whole, 4096, 0x11);
	fill(whole + whole_size - 4096, 4096, 0x22);
	zassert_true(intact(whole, 4096, 0x11), "整块的开头");
	zassert_true(intact(whole + whole_size - 4096, 4096, 0x22),
		     "整块的末尾");
	free(whole);
}

ZTEST(libc_heap, test_single_allocation_limits)
{
	const size_t bytes = entelea_libc_heap_bytes();
	const size_t largest = largest_single();
	size_t count = 0;
	uint8_t *block = malloc(bytes);

	zassert_is_null(block, "一次分配拿不下全部池子");
	zassert_equal(errno, ENOMEM, "拿不下时报 ENOMEM");
	block = malloc(largest);
	zassert_not_null(block, "一次分配可以拿下单个池子的几乎全部");
	fill(block, 8192, 0x11);
	fill(block + largest - 8192, 8192, 0x22);
	zassert_true(intact(block, 8192, 0x11), "开头可写可读");
	zassert_true(intact(block + largest - 8192, 8192, 0x22),
		     "末尾可写可读");
	free(block);
	block = malloc(largest);
	zassert_not_null(block, "归还后可以再次拿下");
	free(block);
	/* 分满后返回空；归还一块后恢复。 */
	for (; count < BLOCK_MAX; count++) {
		blocks[count] = malloc(BLOCK_SIZE);
		if (blocks[count] == NULL) {
			break;
		}
	}
	zassert_true(count >= 2, "至少分出两块");
	zassert_equal(errno, ENOMEM, "分满后报 ENOMEM");
	free(blocks[count - 1]);
	blocks[count - 1] = malloc(BLOCK_SIZE);
	zassert_not_null(blocks[count - 1], "归还一块后可以再次分配");
	for (size_t i = 0; i < count; i++) {
		free(blocks[i]);
		blocks[i] = NULL;
	}
}

ZTEST(libc_heap, test_realloc_keeps_contents)
{
	const size_t bytes = entelea_libc_heap_bytes();
	uint8_t *block = malloc(4096);
	uint8_t *grown;

	zassert_not_null(block, "malloc(4096)");
	fill(block, 4096, 0x33);
	grown = realloc(block, 65536);
	zassert_not_null(grown, "增长");
	zassert_true(entelea_libc_heap_owns(grown), "增长后仍在池子里");
	zassert_true(intact(grown, 4096, 0x33), "增长保留原有内容");
	grown = realloc(grown, 512);
	zassert_not_null(grown, "缩小");
	zassert_true(intact(grown, 512, 0x33), "缩小保留内容");
	zassert_is_null(realloc(grown, 0), "大小为 0 即释放");
	/* 池子放不下的增长：返回空，原块原样保留。 */
	block = malloc(64);
	zassert_not_null(block, "malloc(64)");
	fill(block, 64, 0x44);
	errno = 0;
	grown = realloc(block, bytes);
	zassert_is_null(grown, "放不下的增长返回空");
	zassert_equal(errno, ENOMEM, "放不下时报 ENOMEM");
	zassert_true(intact(block, 64, 0x44), "原块未被改动");
	free(block);
	block = realloc(NULL, 128);
	zassert_not_null(block, "realloc(NULL, n) 等于 malloc(n)");
	free(block);
}

ZTEST(libc_heap, test_entry_points)
{
	uint8_t *aligned = aligned_alloc(4096, 8192);

	zassert_not_null(aligned, "aligned_alloc");
	zassert_equal((uintptr_t) aligned % 4096, 0, "按 4096 对齐");
	zassert_true(entelea_libc_heap_owns(aligned), "对齐分配同样来自池子");
	fill(aligned, 8192, 0x55);
	zassert_true(intact(aligned, 8192, 0x55), "对齐分配可写可读");
	free(aligned);

	uint32_t *zeroed = calloc(256, sizeof(*zeroed));

	zassert_not_null(zeroed, "calloc");
	for (size_t i = 0; i < 256; i++) {
		zassert_equal(zeroed[i], 0, "calloc 清零");
	}
	zeroed = realloc(zeroed, 2048 * sizeof(*zeroed));
	zassert_not_null(zeroed, "calloc 得到的内存可以增长");
	for (size_t i = 0; i < 256; i++) {
		zassert_equal(zeroed[i], 0, "增长保留原有内容");
	}
	free(zeroed);

	char *copy = strdup("entelea-heap");

	zassert_not_null(copy, "strdup");
	zassert_str_equal(copy, "entelea-heap", "strdup 复制内容");
	/* 仿真器链接的是主机 libc，它自己的 strdup 不经过入口；真机上 C 库
	 * 内部的分配都经过。 */
	if (!IS_ENABLED(CONFIG_NATIVE_LIBC)) {
		zassert_true(entelea_libc_heap_owns(copy),
			     "库内部分配同样来自池子");
	}
	free(copy);
	zassert_is_null(malloc(0), "大小为 0 返回空");
	free(NULL);
}

ZTEST(libc_heap, test_memory_taken_before_pools)
{
	uint8_t *grown;

	zassert_not_null(early_block, "池子登记之前的分配成功");
	zassert_false(entelea_libc_heap_owns(early_block), "它不来自池子");
	fill(early_block, 64, 0x66);
	grown = realloc(early_block, 4096);
	zassert_not_null(grown, "原生分配器的增长");
	zassert_false(entelea_libc_heap_owns(grown), "增长后仍归原生分配器");
	zassert_true(intact(grown, 64, 0x66), "内容保留");
	free(grown);
}

K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, WORKERS, 2048);
static struct k_thread worker_threads[WORKERS];
static atomic_t worker_failures;

static void
worker(void *arg1, void *arg2, void *arg3)
{
	uint32_t seed = 1u + (uint32_t) (uintptr_t) arg1;
	uint8_t *slot[4] = { NULL, NULL, NULL, NULL };
	size_t size[4] = { 0 };

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	for (uint32_t round = 0; round < ROUNDS; round++) {
		uint8_t value = (uint8_t) (0x10u + round + seed % 200u);

		for (size_t i = 0; i < 4; i++) {
			seed = seed * 1103515245u + 12345u;
			size[i] = 32u + (seed >> 16) % 16384u;
			slot[i] = malloc(size[i]);
			if (slot[i] == NULL) {
				(void) atomic_inc(&worker_failures);
				continue;
			}
			fill(slot[i], size[i], value);
		}
		/* 让出给同优先级的其它工作线程，制造交错 */
		k_yield();
		if (slot[0] != NULL) {
			size_t grown_size = size[0] + 4096;
			uint8_t *grown = realloc(slot[0], grown_size);

			if (grown == NULL) {
				(void) atomic_inc(&worker_failures);
			} else {
				if (!intact(grown, size[0], value)) {
					(void) atomic_inc(&worker_failures);
				}
				slot[0] = grown;
				size[0] = grown_size;
				fill(slot[0], size[0], value);
			}
		}
		for (size_t i = 0; i < 4; i++) {
			if (slot[i] == NULL) {
				continue;
			}
			if (!intact(slot[i], size[i], value)) {
				(void) atomic_inc(&worker_failures);
			}
			free(slot[i]);
			slot[i] = NULL;
		}
	}
}

ZTEST(libc_heap, test_concurrent_use)
{
	uint8_t *whole;

	atomic_set(&worker_failures, 0);
	for (size_t i = 0; i < WORKERS; i++) {
		k_thread_create(&worker_threads[i], worker_stacks[i],
				K_THREAD_STACK_SIZEOF(worker_stacks[i]), worker,
				(void *) (uintptr_t) i, NULL, NULL,
				K_PRIO_COOP(5), 0, K_NO_WAIT);
	}
	for (size_t i = 0; i < WORKERS; i++) {
		zassert_equal(k_thread_join(&worker_threads[i], K_FOREVER), 0,
			      "工作线程 %zu 结束", i);
	}
	zassert_equal(atomic_get(&worker_failures), 0,
		      "并发分配、增长与释放互不破坏");
	whole = malloc(largest_single());
	zassert_not_null(whole, "并发用例之后可以再次拿下单个池子的几乎全部");
	free(whole);
}

/* 只记数据点：仿真时钟在代码运行时不推进，所以只在真机上量。 */
ZTEST(libc_heap, test_speed_datapoint)
{
	const size_t chunk =
		MIN(entelea_libc_heap_bytes() / 4, 4u * 1024 * 1024);
	uint32_t started;
	uint32_t elapsed;
	uint32_t sum = 0;
	uint8_t *block;

	if (IS_ENABLED(CONFIG_ARCH_POSIX)) {
		return;
	}
	started = k_uptime_get_32();
	for (uint32_t i = 0; i < 1000; i++) {
		uint8_t *small = malloc(1024);

		zassert_not_null(small, "malloc(1024)");
		fill(small, 1024, 1);
		free(small);
	}
	elapsed = k_uptime_get_32() - started;
	printk("[heap] malloc(1024) + memset + free，1000 次：%u 毫秒\n",
	       elapsed);

	block = malloc(chunk);
	zassert_not_null(block, "malloc(%zu)", chunk);
	started = k_uptime_get_32();
	fill(block, chunk, 0x5a);
	elapsed = k_uptime_get_32() - started;
	printk("[heap] 顺序写 %u KB：%u 毫秒\n", (unsigned) (chunk / 1024),
	       elapsed);
	started = k_uptime_get_32();
	for (size_t i = 0; i < chunk; i += 64) {
		sum += block[i];
	}
	elapsed = k_uptime_get_32() - started;
	zassert_equal(sum, (uint32_t) (0x5a * (chunk / 64)), "读回写入的内容");
	printk("[heap] 间隔 64 字节读 %u KB：%u 毫秒\n",
	       (unsigned) (chunk / 1024), elapsed);
	free(block);
}

ZTEST_SUITE(libc_heap, NULL, NULL, NULL, NULL, NULL);
