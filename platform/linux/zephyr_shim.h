/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_LINUX_ZEPHYR_SHIM_H
#define ENTELEA_LINUX_ZEPHYR_SHIM_H
/* Minimal Zephyr kernel-surface shim for the Linux host. Framework mechanism
 * sources compile unmodified on Linux (D02 "same-source contract" discipline:
 * one source set, platform adaptation on both sides).
 * Provides: k_mutex/k_spinlock, k_sleep, k_is_in_isr, irq_lock/unlock,
 * atomic_t, and the K_MSGQ/sysworkq surface. On the Linux host, the event ISR
 * queue degrades to synchronous dispatch (no ISR context); k_work is not
 * brought up on target: the wdt patrol is invoked directly by tests. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

struct k_mutex {
	pthread_mutex_t m;
};
#define Z_MUTEX_INITIALIZER(name) { PTHREAD_MUTEX_INITIALIZER }
struct k_spinlock {
	atomic_bool locked;
};
typedef unsigned int k_spinlock_key_t;
static inline k_spinlock_key_t
k_spin_lock(struct k_spinlock *lock)
{
	while (atomic_exchange_explicit(&lock->locked, true,
					memory_order_acquire)) {
		sched_yield();
	}
	return 0;
}
static inline void
k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	(void) key;
	atomic_store_explicit(&lock->locked, false, memory_order_release);
}
static inline int
k_mutex_lock(struct k_mutex *mu, int64_t timeout)
{
	(void) timeout;
	return pthread_mutex_lock(&mu->m) == 0 ? 0 : -1;
}
static inline int
k_mutex_unlock(struct k_mutex *mu)
{
	return pthread_mutex_unlock(&mu->m) == 0 ? 0 : -1;
}
static inline bool
k_is_in_isr(void)
{
	return false;
}
static inline int64_t
k_sleep(int64_t ms)
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
static inline atomic_val_t
atomic_get(const atomic_t *a)
{
	return *a;
}
static inline void
atomic_set(atomic_t *a, atomic_val_t v)
{
	*a = v;
}
static inline void
atomic_inc(atomic_t *a)
{
	*a = *a + 1;
}
static inline unsigned int
irq_lock(void)
{
	return 0;
}
static inline void
irq_unlock(unsigned int k)
{
	(void) k;
}

#define __ASSERT(cond, msg) ((void) (cond))
#define ARG_UNUSED(x) (void) (x)
#define __weak __attribute__((weak))
#define __ASSERT_NO_MSG(cond) ((void) (cond))

/* events.c queue/work surface (single-threaded synchronous degradation). */
struct k_work {
	char _pad[8];
};
struct k_work_q {
	char _pad[8];
};
struct k_msgq {
	char _pad[8];
};
#define K_MSGQ_DEFINE(name, msg_sz, max, align) struct k_msgq name = { { 0 } }
#define Z_WORK_INITIALIZER(fn) { 0 }
#define K_NO_WAIT 0
int
k_msgq_put(struct k_msgq *q, const void *data, int32_t timeout);
int
k_msgq_get(struct k_msgq *q, void *data, int32_t timeout);
extern struct k_work_q k_sys_work_q;
int
k_work_submit_to_queue(struct k_work_q *q, struct k_work *w);

/* wdt.c work surface (Linux host: patrol_once is called directly by tests;
 * scheduling is a no-op). */
struct k_work_sync {
	char _pad[8];
};
struct k_work_delayable {
	struct k_work work;
};
typedef void (*k_work_handler_t)(struct k_work *);
static inline struct k_work_delayable *
k_work_delayable_from_work(struct k_work *w)
{
	return (struct k_work_delayable *) w;
}
static inline void
k_work_init_delayable(struct k_work_delayable *d, k_work_handler_t fn)
{
	(void) d;
	(void) fn;
}
static inline int
k_work_reschedule_for_queue(struct k_work_q *q, struct k_work_delayable *d,
			    int64_t delay)
{
	(void) q;
	(void) d;
	(void) delay;
	return 0;
}
static inline bool
k_work_cancel_delayable_sync(struct k_work_delayable *d, struct k_work_sync *s)
{
	(void) d;
	(void) s;
	return false;
}

#endif
