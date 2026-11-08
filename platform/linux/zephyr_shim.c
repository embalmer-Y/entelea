/* SPDX-License-Identifier: Apache-2.0 */
/* Linux 宿主垫片实现：events.c 的队列/工作面（单线程同步降级——
 * Linux 宿主无 ISR 发布，队列恒空；k_sys_work_q 为占位）。 */
#include "zephyr_shim.h"
#include <stddef.h>

int k_msgq_put(struct k_msgq *q, const void *data, int32_t timeout)
{
	(void)q; (void)data; (void)timeout;
	return 0;
}

int k_msgq_get(struct k_msgq *q, void *data, int32_t timeout)
{
	(void)q; (void)data; (void)timeout;
	return -11; /* EAGAIN：队列恒空 */
}

struct k_work_q k_sys_work_q;

int k_work_submit_to_queue(struct k_work_q *q, struct k_work *w)
{
	(void)q; (void)w;
	return 0;
}
