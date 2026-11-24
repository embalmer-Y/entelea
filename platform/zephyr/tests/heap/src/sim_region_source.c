/* SPDX-License-Identifier: Apache-2.0 */
/* 仅仿真器：设备树里的地址在主机进程里不是有效指针，声明的区域改由主机内存
 * 提供（大小取设备树的声明）。真机上不编译本文件。 */
#include <stddef.h>
#include <sys/mman.h>

#include <zephyr/mem_mgmt/mem_attr.h>

void *
entelea_libc_heap_region_base(const struct mem_attr_region_t *region)
{
	void *mem = mmap(NULL, region->dt_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return mem == MAP_FAILED ? NULL : mem;
}
