/* SPDX-License-Identifier: Apache-2.0 */
/* D12 闸宿主测试（ENTELEA_TEST）：真实 pthread 互斥与进程内存持久端口下，
 * 多线程并发不超当日余量；重启（复位并重绑，持久记录保留）不重置日额；
 * 墙钟未知拒绝、日界切换。墙钟用受控替身以保证确定性，生产墙钟仅检查可读。 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <entelea/d12.h>
#include <entelea/time.h>
#include "d12_ports.h"

#define DAY0 20000ULL
#define DAY_SECS 86400ULL
#define WORKERS 4
#define ATTEMPTS 50
#define MIN_UNIX_S 1600000000ULL

static int failures;

#define CHECK(cond)                                                    \
	do {                                                           \
		if (!(cond)) {                                         \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,  \
				__LINE__, #cond);                      \
			failures++;                                    \
		}                                                      \
	} while (0)

static uint64_t fake_now;
static uint64_t fake_utc_s = DAY0 * DAY_SECS;
static bool wall_fail;
static entelea_clock_t clk;
static entelea_wallclock_t wall;
static entelea_lock_t lock;
static entelea_d12_store_t store;

static entelea_result_t mono_read(void *user, uint64_t *out)
{
	*out = *(uint64_t *)user;
	return ENTELEA_OK;
}

static entelea_result_t fake_wall_read(void *user, uint64_t *out)
{
	if (wall_fail) {
		return ENTELEA_E_STATE;
	}
	*out = *(uint64_t *)user;
	return ENTELEA_OK;
}

/* 启动装配：每次重启都重绑全部端口；进程内存中的持久记录保留。 */
static void boot_bind(void)
{
	entelea_d12_test_reset();
	CHECK(entelea_clock_init(&clk, mono_read, &fake_now, 1) == ENTELEA_OK);
	CHECK(entelea_d12_bind_clock(&clk) == ENTELEA_OK);
	CHECK(entelea_d12_bind_lock(&lock) == ENTELEA_OK);
	CHECK(entelea_d12_bind_wallclock(&wall) == ENTELEA_OK);
	CHECK(entelea_d12_bind_store(&store) == ENTELEA_OK);
}

/* 预置持久记录（与 d12.c 同格式：magic "D12G"、version 1、flags 0、
 * reserved u16 小端、utc_day u32 小端）。 */
static void seed(uint32_t day, uint16_t reserved)
{
	uint8_t rec[ENTELEA_D12_REC_LEN];

	memcpy(rec, "D12G", 4);
	rec[4] = 1;
	rec[5] = 0;
	rec[6] = (uint8_t)(reserved & 0xFFU);
	rec[7] = (uint8_t)(reserved >> 8);
	rec[8] = (uint8_t)(day & 0xFFU);
	rec[9] = (uint8_t)((day >> 8) & 0xFFU);
	rec[10] = (uint8_t)((day >> 16) & 0xFFU);
	rec[11] = (uint8_t)((day >> 24) & 0xFFU);
	CHECK(store.save(store.user, rec) == ENTELEA_OK);
}

static pthread_mutex_t tally_mu = PTHREAD_MUTEX_INITIALIZER;
static int admitted;
static int refused;

static void *worker(void *arg)
{
	int ok = 0;
	int bad = 0;

	(void)arg;
	for (int i = 0; i < ATTEMPTS; i++) {
		if (entelea_d12_model_allow() == ENTELEA_OK) {
			ok++;
		} else {
			bad++;
		}
	}
	pthread_mutex_lock(&tally_mu);
	admitted += ok;
	refused += bad;
	pthread_mutex_unlock(&tally_mu);
	return NULL;
}

/* 多线程并发：当日余量 4、时间固定（60s 窗另限 6 次），放行必须恰为 4。 */
static void test_concurrent_admission_respects_daily_cap(void)
{
	pthread_t th[WORKERS];

	fake_now = 1000;
	fake_utc_s = DAY0 * DAY_SECS;
	wall_fail = false;
	seed((uint32_t)DAY0, 196);
	boot_bind();
	admitted = 0;
	refused = 0;
	for (int i = 0; i < WORKERS; i++) {
		CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
	}
	for (int i = 0; i < WORKERS; i++) {
		CHECK(pthread_join(th[i], NULL) == 0);
	}
	CHECK(admitted == 4);
	CHECK(refused == WORKERS * ATTEMPTS - 4);
	CHECK(entelea_d12_model_calls() == 4U);
}

/* 每 37 次尝试重启一次：放行绝不超过 200；重启偏保守的损失每次不超过 K−1。 */
static void test_restarts_do_not_reset_daily_cap(void)
{
	int attempts = 0;
	int boots = 0;
	int ok = 0;

	fake_utc_s = (DAY0 + 10) * DAY_SECS;
	wall_fail = false;
	boot_bind();
	while (attempts < 400) {
		fake_now += 60000U;
		attempts++;
		if (entelea_d12_model_allow() == ENTELEA_OK) {
			ok++;
		}
		if (attempts % 37 == 0) {
			boot_bind();
			boots++;
		}
	}
	CHECK(ok <= 200);
	CHECK(ok >= 200 - (int)(ENTELEA_D12_RESERVE_STEP - 1U) * boots);
	fake_now += 60000U;
	CHECK(entelea_d12_model_allow() == ENTELEA_E_BUSY);
}

/* 墙钟未知即拒绝（不计放行）；墙钟恢复后，额度用尽仍拒，次日重新计数。 */
static void test_wall_unknown_and_day_rollover(void)
{
	fake_utc_s = (DAY0 + 20) * DAY_SECS;
	seed((uint32_t)(DAY0 + 20), 200);
	boot_bind();
	wall_fail = true;
	fake_now += 60000U;
	CHECK(entelea_d12_model_allow() == ENTELEA_E_STATE);
	wall_fail = false;
	CHECK(entelea_d12_model_allow() == ENTELEA_E_BUSY);
	fake_utc_s += DAY_SECS;
	CHECK(entelea_d12_model_allow() == ENTELEA_OK);
}

int main(void)
{
	entelea_wallclock_t host_wall;
	uint64_t host_now = 0;

	CHECK(entelea_platform_d12_lock_init(&lock) == ENTELEA_OK);
	CHECK(entelea_platform_d12_store_init(&store) == ENTELEA_OK);
	CHECK(entelea_platform_d12_wallclock_init(&host_wall) == ENTELEA_OK);
	CHECK(host_wall.read_utc_s(host_wall.user, &host_now) == ENTELEA_OK);
	CHECK(host_now > MIN_UNIX_S);
	wall.read_utc_s = fake_wall_read;
	wall.user = &fake_utc_s;

	test_concurrent_admission_respects_daily_cap();
	test_restarts_do_not_reset_daily_cap();
	test_wall_unknown_and_day_rollover();
	if (failures != 0) {
		fprintf(stderr, "d12 host tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("d12 host tests: PASS\n");
	return 0;
}
