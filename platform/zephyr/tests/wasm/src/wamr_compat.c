/* SPDX-License-Identifier: Apache-2.0 */
/* WAMR × Zephyr 4.4 兼容垫片（M2b.2a，零上游补丁纪律）。
 * WAMR-2.4.5 的 zephyr 平台层（core/shared/platform/zephyr/zephyr_platform.c）
 * 调用 Zephyr ≥3.x 已移除的 __stdout_hook_install（旧 printf 钩子机制）；
 * Zephyr 4.4 下 printk 原生可用，钩子无需安装——本垫片仅满足链接符号。
 * picolibc 的 Zephyr 集成自带同名符号（lib/libc/picolibc/stdio.c），垫片仅对
 * 无该实现的 libc（minimal-libc，native_sim 基线）编译（xiao_esp32s3 实证 2026-09-26）。
 * 升级 WAMR 后若平台层不再引用，可移除（撤垫片须重建复测，DR-22 纪律）。 */
#include <zephyr/kernel.h>

#if !defined(CONFIG_PICOLIBC)
void __stdout_hook_install(int (*hook)(int));

void __stdout_hook_install(int (*hook)(int))
{
	ARG_UNUSED(hook);
}
#endif

/* xtensa 软 8 字节原子族（V2 纵深 M19a，DEC-55）：TS_APP_THREADS 开启
 * WASM_ENABLE_SHARED_MEMORY 后解释器 i64.atomic 路径引用 GCC __atomic_*_8
 * 助手；xtensa（32 位）无原生 8 字节原子且 SDK 无 libatomic——irq_lock
 * 关中断实现（单核 V1 语义足够；native_sim/x86 由 GCC 内联展开不经此）。
 * 零上游补丁纪律：垫片于我方模块侧。 */
#if defined(CONFIG_TS_APP_THREADS) && defined(CONFIG_XTENSA)
#include <zephyr/irq.h>

uint64_t __atomic_load_8(const volatile void *ptr, int memorder)
{
	ARG_UNUSED(memorder);
	const volatile uint64_t *p = ptr;
	unsigned int key = irq_lock();
	uint64_t v = *p;

	irq_unlock(key);
	return v;
}

void __atomic_store_8(volatile void *ptr, uint64_t val, int memorder)
{
	ARG_UNUSED(memorder);
	volatile uint64_t *p = ptr;
	unsigned int key = irq_lock();

	*p = val;
	irq_unlock(key);
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder)
{
	ARG_UNUSED(memorder);
	volatile uint64_t *p = ptr;
	unsigned int key = irq_lock();
	uint64_t old = *p;

	*p = val;
	irq_unlock(key);
	return old;
}

bool __atomic_compare_exchange_8(volatile void *ptr, void *expected,
				  uint64_t desired, int weak,
				  int success_mo, int failure_mo)
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
