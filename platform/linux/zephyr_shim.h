/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LINUX_ZEPHYR_SHIM_H
#define ENTELEA_LINUX_ZEPHYR_SHIM_H
/* Linux 宿主的 Zephyr 内核面最小垫片：framework 机制源无修改地在
 * Linux 编译（D02"同源合同"纪律——一套源码、两侧平台适配）。
 * 提供：k_mutex 族、k_sleep、k_is_in_isr、irq_lock/unlock、atomic_t、
 * K_MSGQ/sysworkq 面（事件 ISR 队列在 Linux 宿主降级为同步分发——
 * 无 ISR 上下文；k_work 不上机：wdt 巡检经测试直调）。 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

struct k_mutex { pthread_mutex_t m; };
#define Z_MUTEX_INITIALIZER(name) { PTHREAD_MUTEX_INITIALIZER }
static inline int k_mutex_lock(struct k_mutex *mu, int64_t timeout)
{
	(void)timeout;
	return pthread_mutex_lock(&mu->m) == 0 ? 0 : -1;
}
static inline int k_mutex_unlock(struct k_mutex *mu)
{
	return pthread_mutex_unlock(&mu->m) == 0 ? 0 : -1;
}
static inline bool k_is_in_isr(void) { return false; }
static inline int64_t k_sleep(int64_t ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
	return ms;
}
#define K_FOREVER (-1)
#define K_MSEC(x) (x)
typedef int atomic_val_t;
typedef _Atomic long atomic_t;
#define ATOMIC_INIT(v) (v)
static inline atomic_val_t atomic_get(const atomic_t *a) { return *a; }
static inline void atomic_set(atomic_t *a, atomic_val_t v) { *a = v; }
static inline void atomic_inc(atomic_t *a) { *a = *a + 1; }
static inline unsigned int irq_lock(void) { return 0; }
static inline void irq_unlock(unsigned int k) { (void)k; }

#define __ASSERT(cond, msg) ((void)(cond))
#define ARG_UNUSED(x) (void)(x)
#define __weak __attribute__((weak))
#define __ASSERT_NO_MSG(cond) ((void)(cond))

/* events.c 队列/工作面（单线程同步降级）。 */
struct k_work { char _pad[8]; };
struct k_work_q { char _pad[8]; };
struct k_msgq { char _pad[8]; };
#define K_MSGQ_DEFINE(name, msg_sz, max, align) struct k_msgq name = {{0}}
#define Z_WORK_INITIALIZER(fn) {0}
#define K_NO_WAIT 0
int k_msgq_put(struct k_msgq *q, const void *data, int32_t timeout);
int k_msgq_get(struct k_msgq *q, void *data, int32_t timeout);
extern struct k_work_q k_sys_work_q;
int k_work_submit_to_queue(struct k_work_q *q, struct k_work *w);

/* wdt.c 工作面（Linux 宿主：巡检经测试直调 patrol_once——调度 no-op）。 */
struct k_work_sync { char _pad[8]; };
struct k_work_delayable { struct k_work work; };
typedef void (*k_work_handler_t)(struct k_work *);
static inline struct k_work_delayable *k_work_delayable_from_work(struct k_work *w)
{
	return (struct k_work_delayable *)w;
}
static inline void k_work_init_delayable(struct k_work_delayable *d,
					 k_work_handler_t fn)
{
	(void)d;
	(void)fn;
}
static inline int k_work_reschedule_for_queue(struct k_work_q *q,
					      struct k_work_delayable *d,
					      int64_t delay)
{
	(void)q;
	(void)d;
	(void)delay;
	return 0;
}
static inline bool k_work_cancel_delayable_sync(struct k_work_delayable *d,
						struct k_work_sync *s)
{
	(void)d;
	(void)s;
	return false;
}

#endif
