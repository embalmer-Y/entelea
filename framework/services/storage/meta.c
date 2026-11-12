/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Power-fail-safe meta (migrated from the old store/meta.c; R03 contract
 * in this directory's contract.md):
 * two copies, monotonic sequence number, CRC16. Torn-write recovery: the
 * active copy is the one with the larger sequence number whose CRC passes;
 * if both copies are damaged, ENTELEA_E_IO is returned (no faked default
 * success). Write order: erase the inactive copy's range, write, read back
 * to verify, then it becomes the new active copy (a tear leaves the old copy
 * fully readable).
 * Carried over from the MD1.1b fix: the two copy bodies are read into
 * independent buffers (with a shared buffer, a newer copy0 read back copy1's
 * stale content; boot reproduced this 100% after the 3rd/5th/... write).
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define REC_HDR 8 /* seq u32 | len u16 | crc16 u16 (little-endian) */

static uint32_t
meta_stride(void)
{
	return entelea_store_backend.size(ENTELEA_PART_META) / 2;
}

static entelea_result_t
read_rec(uint8_t copy, uint32_t *seq, uint16_t *len, uint8_t *data)
{
	uint8_t hdr[REC_HDR];
	uint32_t stride = meta_stride();

	if (stride < REC_HDR + ENTELEA_STORAGE_META_MAX ||
	    entelea_store_backend.read(ENTELEA_PART_META, copy * stride, hdr,
				       REC_HDR) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	*seq = entelea_get_le32(hdr);
	*len = entelea_get_le16(hdr + 4);
	uint16_t crc = entelea_get_le16(hdr + 6);

	if (*len == 0 || *len > ENTELEA_STORAGE_META_MAX) {
		return ENTELEA_E_IO;
	}
	if (entelea_store_backend.read(ENTELEA_PART_META,
				       copy * stride + REC_HDR, data,
				       *len) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_crc16(data, *len) != crc) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

static entelea_result_t
write_rec(uint8_t copy, uint32_t seq, const void *buf, uint16_t len)
{
	uint8_t rec[REC_HDR + ENTELEA_STORAGE_META_MAX];
	uint32_t stride = meta_stride();

	if (stride < REC_HDR + len) {
		return ENTELEA_E_RANGE;
	}
	entelea_put_le32(rec, seq);
	entelea_put_le16(rec + 4, len);
	entelea_put_le16(rec + 6, entelea_crc16(buf, len));
	memcpy(rec + REC_HDR, buf, len);
	uint32_t total = REC_HDR + len;

	/* A tear between erase and write damages this copy, while the other
	 * copy stays intact (safety preserved). The RAM backend is harmless
	 * with the same ordering. */
	if (entelea_store_backend.erase_off(ENTELEA_PART_META, copy * stride,
					    stride) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	if (entelea_store_backend.write(ENTELEA_PART_META, copy * stride, rec,
					total) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	/* Verify by read-back */
	uint8_t back[REC_HDR + ENTELEA_STORAGE_META_MAX];

	if (entelea_store_backend.read(ENTELEA_PART_META, copy * stride, back,
				       total) != ENTELEA_OK ||
	    memcmp(rec, back, total) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_storage_meta_write(const void *buf, uint16_t len)
{
	if (buf == NULL || len == 0 || len > ENTELEA_STORAGE_META_MAX) {
		return ENTELEA_E_PARAM;
	}
	uint8_t data[ENTELEA_STORAGE_META_MAX];
	uint32_t s0, s1;
	uint16_t l0, l1;

	bool ok0 = read_rec(0, &s0, &l0, data) == ENTELEA_OK;
	bool ok1 = read_rec(1, &s1, &l1, data) == ENTELEA_OK;

	uint32_t cur_seq = 0;
	uint8_t inactive = 0;

	if (ok0 && ok1) {
		cur_seq = (s0 >= s1) ? s0 : s1;
		inactive = (s0 >= s1) ? 1 : 0;
	} else if (ok0) {
		cur_seq = s0;
		inactive = 1;
	} else if (ok1) {
		cur_seq = s1;
		inactive = 0;
	}
	return write_rec(inactive, cur_seq + 1, buf, len);
}

entelea_result_t
entelea_storage_meta_read(void *buf, uint16_t cap, uint16_t *len)
{
	if (buf == NULL || len == NULL) {
		return ENTELEA_E_PARAM;
	}
	/* Independent buffers per copy (MD1.1b: with a shared buffer, a
	 * newer copy0 read back copy1's old value). */
	uint8_t data0[ENTELEA_STORAGE_META_MAX];
	uint8_t data1[ENTELEA_STORAGE_META_MAX];
	uint32_t s0, s1;
	uint16_t l0, l1;

	bool ok0 = read_rec(0, &s0, &l0, data0) == ENTELEA_OK;
	bool ok1 = read_rec(1, &s1, &l1, data1) == ENTELEA_OK;

	if (!ok0 && !ok1) {
		return ENTELEA_E_IO; /* Both copies bad: fail-safe, no fake */
	}
	const bool use0 = ok0 && (!ok1 || s0 >= s1);
	const uint16_t l = use0 ? l0 : l1;

	if (l > cap) {
		return ENTELEA_E_RANGE; /* No truncation or copy-out */
	}
	memcpy(buf, use0 ? data0 : data1, l);
	*len = l;
	return ENTELEA_OK;
}

entelea_result_t
entelea_storage_meta_read_mirrored(void *buf, uint16_t cap, uint16_t *len)
{
	uint8_t data0[ENTELEA_STORAGE_META_MAX];
	uint8_t data1[ENTELEA_STORAGE_META_MAX];
	uint32_t s0, s1;
	uint16_t l0, l1;

	if (buf == NULL || len == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (read_rec(0, &s0, &l0, data0) != ENTELEA_OK ||
	    read_rec(1, &s1, &l1, data1) != ENTELEA_OK || l0 != l1 ||
	    memcmp(data0, data1, l0) != 0) {
		return ENTELEA_E_IO;
	}
	if (l0 > cap) {
		return ENTELEA_E_RANGE;
	}
	memcpy(buf, data0, l0);
	*len = l0;
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
entelea_result_t
entelea_storage_meta_corrupt_test(uint8_t copy_idx)
{
	if (copy_idx > 1) {
		return ENTELEA_E_PARAM;
	}
	uint32_t stride = meta_stride();

	if (entelea_store_backend.erase_off(ENTELEA_PART_META,
					    copy_idx * stride,
					    stride) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	uint8_t garbage[REC_HDR] = { 0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0 };

	return entelea_store_backend.write(ENTELEA_PART_META, copy_idx * stride,
					   garbage, REC_HDR);
}
#endif
