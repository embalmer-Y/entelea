/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 板级 flash 存储后端（R03 板级批 A-1）：经 Zephyr flash_map 把
 * `storage_partition` 映射为 entelea 八分区命名空间。native_sim 同一
 * 代码路径走 sim-flash（真擦除语义）；真机走 esp32 flash 驱动。
 * 布局：各分区逻辑尺寸在挂载期从 RAM 模板采样冻结（权威值），物理
 * 占用按擦块（4KB）向上取整、顺序紧排。
 * 垫片：写块对齐（P4=4B——头/尾读改写；sim=1B 自然直通）；erase_off
 * 对齐到扇区边界（超集擦除——安全方向）。真断电面归到场项；flash
 * 天然跨 SW/MWDT 复位=本后端的主要收益。
 */
#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <entelea/storage.h>
#include "flash_store.h"

#define SECTOR 4096U
#define BOUNCE_MAX 32U /* 写块垫片缓冲上限（P4=4B；留裕量） */

static const struct flash_area *fa;
static const struct device *flash_dev;
static uint32_t part_off[ENTELEA_PART_COUNT]; /* 物理起始（分区相对） */
static uint32_t part_len[ENTELEA_PART_COUNT]; /* 物理占用（扇区取整） */
static uint32_t logical_size[ENTELEA_PART_COUNT]; /* 挂载期冻结 */
static uint32_t write_blk = 1;

static uint32_t round_up(uint32_t v, uint32_t align)
{
	return (v + align - 1) / align * align;
}

/* 逻辑内偏移 → 物理内偏移。META 特例：逻辑上两副本步长
 * ENTELEA_STORAGE_META_REC（512B）；物理上各占一个擦除扇区——
 * 否则超集擦除（副本 ping-pong 各自 erase_off）会波及同扇区另一
 * 副本（计划符合性板级批发现：两副本共扇区=一次擦除双杀）。 */
static uint32_t phys_off(entelea_store_part_t part, uint32_t off)
{
	if (part == ENTELEA_PART_META &&
	    off >= ENTELEA_STORAGE_META_REC) {
		return SECTOR + (off - ENTELEA_STORAGE_META_REC);
	}
	return off;
}

static uint32_t f_size(entelea_store_part_t part)
{
	return (part >= ENTELEA_PART_COUNT) ? 0 : logical_size[part];
}

static entelea_result_t bounds(entelea_store_part_t part, uint32_t off,
			       uint32_t len)
{
	uint32_t size = f_size(part);

	if (size == 0 || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	return ENTELEA_OK;
}

static entelea_result_t f_read(entelea_store_part_t part, uint32_t off,
			       void *buf, uint32_t len)
{
	entelea_result_t r = bounds(part, off, len);

	if (r != ENTELEA_OK) {
		return r;
	}
	int rc = flash_area_read(fa, part_off[part] + phys_off(part, off), buf,
				 len);

	return rc == 0 ? ENTELEA_OK : ENTELEA_E_IO;
}

/* 写块对齐垫片：头/尾不足一块的部分读-改-写（bounce）；主体直写。 */
static entelea_result_t aligned_write(uint32_t addr, const uint8_t *src,
				      uint32_t len)
{
	uint8_t bounce[BOUNCE_MAX];

	while (len > 0) {
		uint32_t head = addr % write_blk;

		if (head != 0 || len < write_blk) {
			uint32_t start = addr - head;
			uint32_t span = write_blk - head;

			if (span > len) {
				span = len;
			}
			if (flash_area_read(fa, start, bounce, write_blk) != 0) {
				return ENTELEA_E_IO;
			}
			memcpy(bounce + head, src, span);
			if (flash_area_write(fa, start, bounce, write_blk) != 0) {
				return ENTELEA_E_IO;
			}
			addr = start + write_blk;
			src += span;
			len -= span;
			continue;
		}
		uint32_t chunk = len - (len % write_blk);

		if (flash_area_write(fa, addr, src, chunk) != 0) {
			return ENTELEA_E_IO;
		}
		addr += chunk;
		src += chunk;
		len -= chunk;
	}
	return ENTELEA_OK;
}

static entelea_result_t f_write(entelea_store_part_t part, uint32_t off,
				const void *buf, uint32_t len)
{
	entelea_result_t r = bounds(part, off, len);

	if (r != ENTELEA_OK) {
		return r;
	}
	if (len == 0) {
		return ENTELEA_OK;
	}
	return aligned_write(part_off[part] + phys_off(part, off), buf, len);
}

static entelea_result_t f_erase(entelea_store_part_t part)
{
	if (part >= ENTELEA_PART_COUNT) {
		return ENTELEA_E_PARAM;
	}
	/* 整区擦（物理占用=扇区取整——垫片属本区，不越邻区）。 */
	int rc = flash_area_erase(fa, part_off[part], part_len[part]);

	return rc == 0 ? ENTELEA_OK : ENTELEA_E_IO;
}

static entelea_result_t f_erase_off(entelea_store_part_t part, uint32_t off,
				    uint32_t len)
{
	entelea_result_t r = bounds(part, off, len);

	if (r != ENTELEA_OK) {
		return r;
	}
	if (len == 0) {
		return ENTELEA_OK;
	}
	/* 超集擦除：向下取整扇区起点、向上取整长度（NOR 擦除粒度）。
	 * 超集只影响本区垫片（区物理占用扇区对齐；META 经 phys_off
	 * 重映射后副本各自落独立扇区——超集=副本自身扇区）。 */
	uint32_t po = phys_off(part, off);
	uint32_t pl = phys_off(part, off + len - 1) - po + 1;
	uint32_t start = part_off[part] + (po / SECTOR) * SECTOR;
	uint32_t end = part_off[part] + round_up(po + pl, SECTOR);
	int rc = flash_area_erase(fa, start, end - start);

	return rc == 0 ? ENTELEA_OK : ENTELEA_E_IO;
}

static const entelea_store_ops_t flash_ops = {
	.read = f_read,
	.write = f_write,
	.erase = f_erase,
	.erase_off = f_erase_off,
	.size = f_size,
};

entelea_result_t entelea_platform_storage_mount_flash(void)
{
	if (fa != NULL) {
		return ENTELEA_OK; /* 幂等 */
	}
	if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) != 0) {
		return ENTELEA_E_UNINIT;
	}
	flash_dev = flash_area_get_device(fa);
	if (flash_dev == NULL) {
		flash_area_close(fa);
		fa = NULL;
		return ENTELEA_E_UNINIT;
	}
	write_blk = flash_get_write_block_size(flash_dev);
	if (write_blk == 0 || write_blk > BOUNCE_MAX) {
		flash_area_close(fa);
		fa = NULL;
		return ENTELEA_E_NOMEM; /* 写块超垫片缓冲——板配置异常 */
	}
	/* 布局映射：此刻后端仍=RAM——逻辑尺寸即 RAM 权威值，采样冻结。 */
	uint32_t cursor = 0;

	for (int p = 0; p < ENTELEA_PART_COUNT; p++) {
		uint32_t logical = entelea_storage_part_size(
			(entelea_store_part_t)p);

		if (logical == 0) {
			flash_area_close(fa);
			fa = NULL;
			return ENTELEA_E_PARAM;
		}
		logical_size[p] = logical;
		part_off[p] = cursor;
		part_len[p] = round_up(logical, SECTOR);
		if (p == ENTELEA_PART_META) {
			/* 双副本各占一扇区（物理 8K > 逻辑 1K）。 */
			part_len[p] = 2 * SECTOR;
		}
		cursor += part_len[p];
	}
	if (cursor > fa->fa_size) {
		flash_area_close(fa);
		fa = NULL;
		return ENTELEA_E_NOMEM; /* storage 分区装不下八区布局 */
	}
	return entelea_storage_mount(&flash_ops);
}
