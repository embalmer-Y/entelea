/* SPDX-License-Identifier: Apache-2.0 */
/* R07 Linux 最小宿主时钟适配：POSIX CLOCK_MONOTONIC 原始读（ISR 语义
 * 不适用——Linux 宿主无 ISR 喂狗面；与 Zephyr 适配同合同：实例时钟、
 * 原始读、单调性由时钟源保证）。 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <entelea/time.h>

#define ENTELEA_PLATFORM_CLOCK_ID 2U

static entelea_result_t mono_read_ms(void *user, uint64_t *out)
{
	(void)user;
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return ENTELEA_E_IO;
	}
	*out = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
	return ENTELEA_OK;
}

entelea_result_t entelea_platform_clock_init(entelea_clock_t *clock)
{
	return entelea_clock_init(clock, mono_read_ms, NULL,
				  ENTELEA_PLATFORM_CLOCK_ID);
}
