/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Board-level flash storage backend (R03 board batch A-1): maps
 * `storage_partition` to the eight-partition entelea namespace via Zephyr
 * flash_map. native_sim runs the same code path on sim-flash (real erase
 * semantics); real boards use the esp32 flash driver.
 * Layout: each partition's logical size is sampled from the RAM template at
 * mount time and frozen (authoritative values); the physical footprint is
 * rounded up to erase blocks (4 KB) and packed sequentially.
 * Shims: write-block alignment (P4 = 4 B: head/tail read-modify-write;
 * sim = 1 B, passed straight through); erase_off is aligned to sector
 * boundaries (superset erase, the safe direction). Real power-loss coverage
 * is a field item; flash persisting across SW/MWDT resets is this backend's
 * main benefit.
 */
#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <entelea/storage.h>
#include "flash_store.h"

#define SECTOR 4096U
#define BOUNCE_MAX 32U /* write-block bounce cap (P4=4B; headroom) */

static const struct flash_area *fa;
static const struct device *flash_dev;
static uint32_t part_off[ENTELEA_PART_COUNT]; /* start (partition-relative) */
static uint32_t part_len[ENTELEA_PART_COUNT]; /* footprint (sector-rounded) */
static uint32_t logical_size[ENTELEA_PART_COUNT]; /* frozen at mount */
static uint32_t write_blk = 1;

static uint32_t
round_up(uint32_t v, uint32_t align)
{
	return (v + align - 1) / align * align;
}

/* Logical-to-physical offset mapping. META special case: logically the two
 * copies are ENTELEA_STORAGE_META_REC (512 B) apart, while physically each
 * occupies its own erase sector. Otherwise a superset erase (each copy's
 * ping-pong erase_off) would hit the other copy in the same sector (found in
 * the plan-conformance board batch: two copies sharing a sector means one
 * erase kills both). */
static uint32_t
phys_off(entelea_store_part_t part, uint32_t off)
{
	if (part == ENTELEA_PART_META && off >= ENTELEA_STORAGE_META_REC) {
		return SECTOR + (off - ENTELEA_STORAGE_META_REC);
	}
	return off;
}

static uint32_t
f_size(entelea_store_part_t part)
{
	return (part >= ENTELEA_PART_COUNT) ? 0 : logical_size[part];
}

static entelea_result_t
bounds(entelea_store_part_t part, uint32_t off, uint32_t len)
{
	uint32_t size = f_size(part);

	if (size == 0 || off > size || len > size - off) {
		return ENTELEA_E_RANGE;
	}
	return ENTELEA_OK;
}

static entelea_result_t
f_read(entelea_store_part_t part, uint32_t off, void *buf, uint32_t len);

/* META accesses crossing the logical copy boundary (review P2-1) are split
 * into two segments, each translated separately. The old code only remapped
 * the start offset, so a read or write crossing the 512 B boundary landed
 * copy1's head in the physical shim (reads got 0xFF, writes were silently
 * lost). */
static entelea_result_t
f_read(entelea_store_part_t part, uint32_t off, void *buf, uint32_t len)
{
	entelea_result_t r = bounds(part, off, len);

	if (r != ENTELEA_OK) {
		return r;
	}
	if (part == ENTELEA_PART_META && off < ENTELEA_STORAGE_META_REC &&
	    off + len > ENTELEA_STORAGE_META_REC) {
		uint32_t head = ENTELEA_STORAGE_META_REC - off;

		r = f_read(part, off, buf, head);
		if (r != ENTELEA_OK) {
			return r;
		}
		return f_read(part, ENTELEA_STORAGE_META_REC,
			      (uint8_t *) buf + head, len - head);
	}
	int rc = flash_area_read(fa, part_off[part] + phys_off(part, off), buf,
				 len);

	return rc == 0 ? ENTELEA_OK : ENTELEA_E_IO;
}

/* Write-block alignment shim: partial head/tail blocks are
 * read-modify-written through a bounce buffer; the body is written
 * directly. */
static entelea_result_t
aligned_write(uint32_t addr, const uint8_t *src, uint32_t len)
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
			if (flash_area_read(fa, start, bounce, write_blk) !=
			    0) {
				return ENTELEA_E_IO;
			}
			memcpy(bounce + head, src, span);
			if (flash_area_write(fa, start, bounce, write_blk) !=
			    0) {
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

static entelea_result_t
f_write(entelea_store_part_t part, uint32_t off, const void *buf, uint32_t len);

static entelea_result_t
f_write(entelea_store_part_t part, uint32_t off, const void *buf, uint32_t len)
{
	entelea_result_t r = bounds(part, off, len);

	if (r != ENTELEA_OK) {
		return r;
	}
	if (len == 0) {
		return ENTELEA_OK;
	}
	if (part == ENTELEA_PART_META && off < ENTELEA_STORAGE_META_REC &&
	    off + len > ENTELEA_STORAGE_META_REC) {
		uint32_t head = ENTELEA_STORAGE_META_REC - off;

		r = f_write(part, off, buf, head);
		if (r != ENTELEA_OK) {
			return r;
		}
		return f_write(part, ENTELEA_STORAGE_META_REC,
			       (const uint8_t *) buf + head, len - head);
	}
	return aligned_write(part_off[part] + phys_off(part, off), buf, len);
}

static entelea_result_t
f_erase(entelea_store_part_t part)
{
	if (part >= ENTELEA_PART_COUNT) {
		return ENTELEA_E_PARAM;
	}
	/* Whole-partition erase (footprint rounded to sectors; the shim stays
	 * within this partition and never touches neighbors). */
	int rc = flash_area_erase(fa, part_off[part], part_len[part]);

	return rc == 0 ? ENTELEA_OK : ENTELEA_E_IO;
}

static entelea_result_t
f_erase_off(entelea_store_part_t part, uint32_t off, uint32_t len)
{
	entelea_result_t r = bounds(part, off, len);

	if (r != ENTELEA_OK) {
		return r;
	}
	if (len == 0) {
		return ENTELEA_OK;
	}
	/* Superset erase: round the start down to a sector and the length up
	 * (NOR erase granularity). The superset only affects this partition's
	 * shim; the footprint is sector-aligned, and META goes through
	 * phys_off so each copy lands in its own sector (the superset is that
	 * copy's sector). */
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

entelea_result_t
entelea_platform_storage_mount_flash(void)
{
	if (fa != NULL) {
		return ENTELEA_OK; /* idempotent */
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
		return ENTELEA_E_NOMEM; /* write block > bounce: bad config */
	}
	/* Layout mapping: the backend is still RAM here, so logical sizes
	 * are the authoritative RAM values; sample and freeze them. */
	uint32_t cursor = 0;

	for (int p = 0; p < ENTELEA_PART_COUNT; p++) {
		uint32_t logical =
			entelea_storage_part_size((entelea_store_part_t) p);

		if (logical == 0) {
			flash_area_close(fa);
			fa = NULL;
			return ENTELEA_E_PARAM;
		}
		logical_size[p] = logical;
		part_off[p] = cursor;
		part_len[p] = round_up(logical, SECTOR);
		if (p == ENTELEA_PART_META) {
			/* one sector per copy (8K physical, 1K logical) */
			part_len[p] = 2 * SECTOR;
		}
		cursor += part_len[p];
	}
	if (cursor > fa->fa_size) {
		flash_area_close(fa);
		fa = NULL;
		return ENTELEA_E_NOMEM; /* storage too small for eight areas */
	}
	return entelea_storage_mount(&flash_ops);
}
