/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 分区命名空间与后端端口（迁自旧 store/part.c 的 RAM 后端子集，R03 合同
 * framework/services/storage/contract.md）。
 * RAM 后端（native_sim/主机测试）：静态数组 + 0xFF 抹除语义，
 * test_reset 模拟"掉电后全新镜像"。flash 后端（DT fixed-partitions +
 * flash_map，含写对齐垫片）随板级批（D12 冻结后）迁移——本子闭环不提供，
 * 不宣称板级持久化已验证。越界（含 off+len u32 回绕）显式拒绝。
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define META_SIZE 1024U   /* 双副本各 512B */
#define NOINIT_SIZE 256U
#define AUDIT_SIZE 8192U  /* 两页乒乓 × 64 槽 × 64B（DEC-52①） */
#define PROV_SIZE 4096U

static uint8_t ram_meta[META_SIZE];
static uint8_t ram_noinit[NOINIT_SIZE];
static uint8_t ram_audit[AUDIT_SIZE];
static uint8_t ram_prov[PROV_SIZE];

static uint8_t *part_ram(entelea_store_part_t part, uint32_t *size)
{
	switch (part) {
	case ENTELEA_PART_META:
		*size = META_SIZE;
		return ram_meta;
	case ENTELEA_PART_NOINIT:
		*size = NOINIT_SIZE;
		return ram_noinit;
	case ENTELEA_PART_AUDIT:
		*size = AUDIT_SIZE;
		return ram_audit;
	case ENTELEA_PART_PROV:
		*size = PROV_SIZE;
		return ram_prov;
	default:
		*size = 0;
		return NULL;
	}
}

static entelea_result_t ram_read(entelea_store_part_t part, uint32_t off, void *buf,
				 uint32_t len)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	memcpy(buf, base + off, len);
	return ENTELEA_OK;
}

static entelea_result_t ram_write(entelea_store_part_t part, uint32_t off,
				  const void *buf, uint32_t len)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	memcpy(base + off, buf, len);
	return ENTELEA_OK;
}

static entelea_result_t ram_erase_off(entelea_store_part_t part, uint32_t off,
				      uint32_t len)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	memset(base + off, 0xFF, len);
	return ENTELEA_OK;
}

static entelea_result_t ram_erase(entelea_store_part_t part)
{
	uint32_t size;
	uint8_t *base = part_ram(part, &size);

	if (base == NULL) {
		return ENTELEA_E_PARAM;
	}
	memset(base, 0xFF, size);
	return ENTELEA_OK;
}

static uint32_t ram_size(entelea_store_part_t part)
{
	uint32_t size;

	return part_ram(part, &size) == NULL ? 0 : size;
}

const entelea_store_ops_t entelea_store_backend = {
	.read = ram_read,
	.write = ram_write,
	.erase = ram_erase,
	.erase_off = ram_erase_off,
	.size = ram_size,
};

#ifdef ENTELEA_TEST
/* 越界/回绕负测探针：直通后端 read（端口边界合同）。 */
entelea_result_t entelea_storage_test_backend_read(entelea_store_part_t part,
						   uint32_t off, void *buf,
						   uint32_t len)
{
	return entelea_store_backend.read(part, off, buf, len);
}

entelea_result_t entelea_storage_test_backend_write(entelea_store_part_t part,
						    uint32_t off, const void *buf,
						    uint32_t len)
{
	return entelea_store_backend.write(part, off, buf, len);
}

entelea_result_t entelea_storage_test_reset(void)
{
	for (int p = 0; p < ENTELEA_PART_COUNT; p++) {
		if (entelea_store_backend.size((entelea_store_part_t)p) == 0) {
			continue;
		}
		entelea_result_t r = entelea_store_backend.erase((entelea_store_part_t)p);

		if (r != ENTELEA_OK) {
			return r;
		}
	}
	return ENTELEA_OK;
}
#endif

/* 公共端口原语（后端表保持模块私有；跨模块消费经此窄面）。 */
entelea_result_t entelea_storage_read(entelea_store_part_t part, uint32_t off,
				      void *buf, uint32_t len)
{
	return ram_read(part, off, buf, len);
}

entelea_result_t entelea_storage_write(entelea_store_part_t part, uint32_t off,
				       const void *buf, uint32_t len)
{
	return ram_write(part, off, buf, len);
}

entelea_result_t entelea_storage_erase(entelea_store_part_t part)
{
	return ram_erase(part);
}

uint32_t entelea_storage_part_size(entelea_store_part_t part)
{
	return ram_size(part);
}
