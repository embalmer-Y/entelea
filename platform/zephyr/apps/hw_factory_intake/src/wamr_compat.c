/* SPDX-License-Identifier: Apache-2.0 */
/* Byte-for-byte synced with
 * platform/zephyr/tests/wasm/src/wamr_compat.c (review P3-6 note: drift is
 * exposed by the gate/test dual-build surfaces). */
/* WAMR x Zephyr 4.4 compatibility shim (M2b.2a; zero upstream-patch
 * discipline).
 * WAMR-2.4.5's zephyr platform layer
 * (core/shared/platform/zephyr/zephyr_platform.c) calls
 * __stdout_hook_install, which Zephyr >= 3.x has removed (the legacy printf
 * hook mechanism). On Zephyr 4.4, printk works natively and no hook needs to
 * be installed; this shim only satisfies the link symbol. picolibc's Zephyr
 * integration provides the same symbol (lib/libc/picolibc/stdio.c), so the
 * shim is compiled only for libcs without that implementation (minimal-libc,
 * native_sim baseline; verified on xiao_esp32s3, 2026-09-26).
 * Once WAMR no longer references it from the platform layer, the shim may be
 * removed (removal requires a rebuild and retest; DR-22 discipline). */
#include <zephyr/kernel.h>

#if !defined(CONFIG_PICOLIBC)
void
__stdout_hook_install(int (*hook)(int));

void
__stdout_hook_install(int (*hook)(int))
{
	ARG_UNUSED(hook);
}
#endif

/* xtensa soft 8-byte atomic family (V2 depth track M19a, DEC-55): with
 * TS_APP_THREADS enabled, WASM_ENABLE_SHARED_MEMORY makes the interpreter's
 * i64.atomic path reference the GCC __atomic_*_8 helpers. xtensa (32-bit) has
 * no native 8-byte atomics and the SDK has no libatomic, so these are
 * implemented by masking interrupts with irq_lock (sufficient for single-core
 * V1 semantics; native_sim/x86 are expanded inline by GCC and do not use this
 * path). Zero upstream-patch discipline: the shim lives on our module side. */
#if defined(CONFIG_TS_APP_THREADS) && defined(CONFIG_XTENSA)
#include <zephyr/irq.h>

uint64_t
__atomic_load_8(const volatile void *ptr, int memorder)
{
	ARG_UNUSED(memorder);
	const volatile uint64_t *p = ptr;
	unsigned int key = irq_lock();
	uint64_t v = *p;

	irq_unlock(key);
	return v;
}

void
__atomic_store_8(volatile void *ptr, uint64_t val, int memorder)
{
	ARG_UNUSED(memorder);
	volatile uint64_t *p = ptr;
	unsigned int key = irq_lock();

	*p = val;
	irq_unlock(key);
}

uint64_t
__atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder)
{
	ARG_UNUSED(memorder);
	volatile uint64_t *p = ptr;
	unsigned int key = irq_lock();
	uint64_t old = *p;

	*p = val;
	irq_unlock(key);
	return old;
}

bool
__atomic_compare_exchange_8(volatile void *ptr, void *expected,
			    uint64_t desired, int weak, int success_mo,
			    int failure_mo)
{
	ARG_UNUSED(weak);
	ARG_UNUSED(success_mo);
	ARG_UNUSED(failure_mo);
	volatile uint64_t *p = ptr;
	uint64_t *exp = expected;
	unsigned int key = irq_lock();
	bool ok = *p == *exp;

	if (ok) {
		*p = desired;
	} else {
		*exp = *p;
	}
	irq_unlock(key);
	return ok;
}
#endif
