/* SPDX-License-Identifier: Apache-2.0 */
/* Linux host shim implementation: the events.c queue/work surface, degraded
 * to single-threaded synchronous operation. The Linux host has no ISR
 * publishing, so the queue is always empty; k_sys_work_q is a placeholder. */
#include "zephyr_shim.h"
#include <stddef.h>

int
k_msgq_put(struct k_msgq *q, const void *data, int32_t timeout)
{
	(void) q;
	(void) data;
	(void) timeout;
	return 0;
}

int
k_msgq_get(struct k_msgq *q, void *data, int32_t timeout)
{
	(void) q;
	(void) data;
	(void) timeout;
	return -11; /* EAGAIN: queue is always empty */
}

struct k_work_q k_sys_work_q;

int
k_work_submit_to_queue(struct k_work_q *q, struct k_work *w)
{
	(void) q;
	(void) w;
	return 0;
}
