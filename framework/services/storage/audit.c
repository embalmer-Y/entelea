/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Audit two-page ping-pong ring (migrated from the old store/audit.c; R03
 * contract in this directory's contract.md): power-loss retention of
 * security exception classes. 8KB = two pages x 4KB (erase-block aligned) x
 * 64 slots x 64B fixed size. "Overwrite the oldest in place" is not feasible
 * (the erase unit is a page), so two pages ping-pong: when the active page
 * fills, switch to the other page and continue writing (the target page is
 * erased before its first write). Power loss always retains the latest ~64
 * entries (at most 128 during a page switch).
 * RF1 (review P1-6): body and CRC, then the original magic, then an
 * independent commit tag, written in that order. Uncommitted slots are
 * skipped; committed but corrupt slots are reported explicitly. A read error
 * makes init not ready and read return a negative error code; a failed write
 * burns that slot and its seq.
 * Foreign content (owner 2026-11-24): a slot carrying no marker of this ring
 * (another firmware's data in the partition) is not a record of this ring.
 * It is invisible like an uncommitted slot and is never reused; it does not
 * stop the boot and does not erase the records that are ours. It goes away
 * when its page is erased for the next write. An earlier version also
 * required a non-erased magic; on the board that exposed this, 4 of the 128
 * foreign slots started with the erased pattern.
 * Adjustments from the old implementation: boot printing stays in the
 * platform layer (this module is printk-free and is consumed through the
 * read API). The full in-memory ring (old commit.c surface) belongs to the
 * R04 observation surface; this ring only holds exception classes.
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define AUDIT_MAGIC 0x54534131U /* "TSA1" */
#define AUDIT_PAGE_SZ 4096U
#define AUDIT_SLOT_SZ 64U
#define AUDIT_SLOTS (AUDIT_PAGE_SZ / AUDIT_SLOT_SZ) /* 64 */
#define AUDIT_PAGES 2U

static struct {
	bool ready;
	uint8_t active_page; /* 0/1 */
	uint8_t next_slot; /* next write slot in active page, 0..63 */
	uint32_t next_seq; /* global monotonic (from 1; 0/0xFFFFFFFF unused) */
	unsigned foreign; /* slots of foreign content seen by the last scan */
} au;

/* Slot layout (64B, little-endian):
 * magic|seq|t_ms|ch_idx|actor|kind|_pad|evt|res|value|crc16|tag|commit|pad.
 * Legacy slots have erased crc/tag/commit values. */
#define OFF_MAGIC 0
#define OFF_SEQ 4
#define OFF_TMS 8
#define OFF_CHIDX 16
#define OFF_ACTOR 18
#define OFF_KIND 20
#define OFF_PAD1 21
#define OFF_EVT 22
#define OFF_RES 24
#define OFF_VALUE 28
#define OFF_CRC 32
#define OFF_TAG 34
#define OFF_COMMIT 36
#define AUDIT_TAG_MAGIC 0xA56CU
#define AUDIT_TAG_COMMIT 0x31435641U

enum slot_kind {
	SLOT_INCOMPLETE,
	SLOT_VALID,
	SLOT_CORRUPT,
	SLOT_FOREIGN,
};

static void
put_le64(uint8_t *p, uint64_t v)
{
	entelea_put_le32(p, (uint32_t) v);
	entelea_put_le32(p + 4, (uint32_t) (v >> 32));
}

static void
slot_encode(uint8_t *s, const entelea_storage_audit_entry_t *e, uint16_t evt,
	    uint32_t seq)
{
	memset(s, 0xFF, AUDIT_SLOT_SZ); /* pad = erased state */
	entelea_put_le32(s + OFF_MAGIC, AUDIT_MAGIC);
	entelea_put_le32(s + OFF_SEQ, seq);
	put_le64(s + OFF_TMS, e->t_ms);
	entelea_put_le16(s + OFF_CHIDX, e->ch_idx);
	entelea_put_le16(s + OFF_ACTOR, e->actor);
	s[OFF_KIND] = e->kind;
	s[OFF_PAD1] = 0xFF;
	entelea_put_le16(s + OFF_EVT, evt);
	entelea_put_le32(s + OFF_RES, (uint32_t) (int32_t) e->res);
	entelea_put_le32(s + OFF_VALUE, e->value_u);
	entelea_put_le16(s + OFF_CRC,
			 entelea_crc16(s + OFF_SEQ, OFF_CRC - OFF_SEQ));
	entelea_put_le16(s + OFF_TAG, AUDIT_TAG_MAGIC);
	entelea_put_le32(s + OFF_COMMIT, AUDIT_TAG_COMMIT);
}

static void
slot_decode(const uint8_t *s, entelea_storage_audit_entry_t *e)
{
	e->seq = entelea_get_le32(s + OFF_SEQ);
	e->t_ms = (uint64_t) entelea_get_le32(s + OFF_TMS) |
		  ((uint64_t) entelea_get_le32(s + OFF_TMS + 4) << 32);
	e->ch_idx = entelea_get_le16(s + OFF_CHIDX);
	e->actor = entelea_get_le16(s + OFF_ACTOR);
	e->kind = s[OFF_KIND];
	e->res = (entelea_result_t) (int32_t) entelea_get_le32(s + OFF_RES);
	e->evt = entelea_get_le16(s + OFF_EVT);
	e->value_u = entelea_get_le32(s + OFF_VALUE);
}

static entelea_result_t
slot_read(uint8_t page, uint8_t i, uint8_t *s)
{
	return entelea_store_backend.read(ENTELEA_PART_AUDIT,
					  page * AUDIT_PAGE_SZ +
						  i * AUDIT_SLOT_SZ,
					  s, AUDIT_SLOT_SZ) == ENTELEA_OK ?
		       ENTELEA_OK :
		       ENTELEA_E_IO;
}

static bool
slot_erased(const uint8_t *s)
{
	for (uint32_t k = 0; k < AUDIT_SLOT_SZ; k++) {
		if (s[k] != 0xFF) {
			return false;
		}
	}
	return true;
}

static enum slot_kind
slot_kind(const uint8_t *s)
{
	uint32_t seq = entelea_get_le32(s + OFF_SEQ);
	uint32_t commit = entelea_get_le32(s + OFF_COMMIT);
	uint32_t magic = entelea_get_le32(s + OFF_MAGIC);
	bool fields_valid =
		magic == AUDIT_MAGIC && seq != 0 && seq != UINT32_MAX;
	bool legacy = entelea_get_le32(s + OFF_CRC) == UINT32_MAX &&
		      commit == UINT32_MAX;

	/* Foreign: none of the three markers of this ring is present. Content
	 * like that was not written by this ring (for example another
	 * firmware's data in the partition; owner 2026-11-24). The only slots
	 * of ours without a marker are erased ones and writes cut short before
	 * the tag was written; those need the same treatment. Any marker
	 * present makes the slot ours, and a damaged record of ours stays
	 * SLOT_CORRUPT. */
	if (magic != AUDIT_MAGIC &&
	    entelea_get_le16(s + OFF_TAG) != AUDIT_TAG_MAGIC &&
	    commit != AUDIT_TAG_COMMIT) {
		return slot_erased(s) ? SLOT_INCOMPLETE : SLOT_FOREIGN;
	}
	if (legacy) {
		return fields_valid ? SLOT_VALID : SLOT_INCOMPLETE;
	}
	if (commit == UINT32_MAX) {
		return SLOT_INCOMPLETE;
	}
	if (commit != AUDIT_TAG_COMMIT ||
	    entelea_get_le16(s + OFF_TAG) != AUDIT_TAG_MAGIC || !fields_valid ||
	    entelea_get_le16(s + OFF_CRC) !=
		    entelea_crc16(s + OFF_SEQ, OFF_CRC - OFF_SEQ)) {
		return SLOT_CORRUPT;
	}
	return SLOT_VALID;
}

/* A write of ours cut short by power loss: the body goes in first, so the
 * magic and the never-written tail are still erased. Such a slot is not
 * foreign content and is not reported as such. */
static bool
cut_short_write(const uint8_t *s)
{
	if (entelea_get_le32(s + OFF_MAGIC) != UINT32_MAX) {
		return false;
	}
	for (uint32_t k = OFF_COMMIT + 4; k < AUDIT_SLOT_SZ; k++) {
		if (s[k] != 0xFF) {
			return false;
		}
	}
	return true;
}

static entelea_result_t
slot_commit(uint8_t page, uint8_t i, const uint8_t *s)
{
	uint32_t off = page * AUDIT_PAGE_SZ + i * AUDIT_SLOT_SZ;
	uint8_t back[AUDIT_SLOT_SZ];

	if (entelea_store_backend.write(ENTELEA_PART_AUDIT, off + OFF_SEQ,
					s + OFF_SEQ,
					OFF_COMMIT - OFF_SEQ) != ENTELEA_OK ||
	    entelea_store_backend.write(ENTELEA_PART_AUDIT, off, s, OFF_SEQ) !=
		    ENTELEA_OK ||
	    entelea_store_backend.write(ENTELEA_PART_AUDIT, off + OFF_COMMIT,
					s + OFF_COMMIT, 4) != ENTELEA_OK ||
	    slot_read(page, i, back) != ENTELEA_OK ||
	    memcmp(back, s, sizeof(back)) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

/* Page scan: max valid seq plus used-slot count (last non-erased slot + 1;
 * bad and foreign slots are neither reused nor truncate the tail; valid slots
 * after them stay visible). Read errors fail explicitly. */
static entelea_result_t
page_scan(uint8_t page, uint32_t *max_seq, uint8_t *used_slots)
{
	uint8_t s[AUDIT_SLOT_SZ];

	*max_seq = 0;
	*used_slots = 0;
	for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
		if (slot_read(page, i, s) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		enum slot_kind kind = slot_kind(s);

		if (kind == SLOT_CORRUPT) {
			return ENTELEA_E_IO;
		}
		if (kind == SLOT_FOREIGN && !cut_short_write(s)) {
			au.foreign++;
		}
		if (kind == SLOT_VALID &&
		    entelea_get_le32(s + OFF_SEQ) > *max_seq) {
			*max_seq = entelea_get_le32(s + OFF_SEQ);
		}
		if (!slot_erased(s)) {
			*used_slots = (uint8_t) (i + 1);
		}
	}
	return ENTELEA_OK;
}

entelea_result_t
entelea_storage_audit_init(void)
{
	uint32_t seq_a;
	uint32_t seq_b;
	uint8_t used_a;
	uint8_t used_b;

	au.ready = false;
	au.foreign = 0;
	if (page_scan(0, &seq_a, &used_a) != ENTELEA_OK ||
	    page_scan(1, &seq_b, &used_b) != ENTELEA_OK) {
		return ENTELEA_E_IO; /* Read error != empty ring: not ready */
	}
	uint8_t page = (seq_b > seq_a) ? 1U : 0U;
	uint8_t used = page ? used_b : used_a;

	au.next_seq = ((seq_a > seq_b) ? seq_a : seq_b) + 1U;
	if (used >= AUDIT_SLOTS) {
		page ^= 1U; /* Full page: erase target before first append */
		used = 0;
	}
	au.active_page = page;
	au.next_slot = used;
	au.ready = true;
	return ENTELEA_OK;
}

unsigned
entelea_storage_audit_foreign_slots(void)
{
	return au.foreign;
}

entelea_result_t
entelea_storage_audit_append(const entelea_storage_audit_entry_t *e,
			     uint16_t evt)
{
	if (e == NULL || !au.ready || au.next_seq == UINT32_MAX) {
		return ENTELEA_E_STATE;
	}
	if (au.next_slot == 0) {
		/* First write on a page: ensure the target page is erased
		 * (recovery path for power loss during a page switch;
		 * erase is idempotent; a failure consumes no slot or seq). */
		if (entelea_store_backend.erase_off(
			    ENTELEA_PART_AUDIT, au.active_page * AUDIT_PAGE_SZ,
			    AUDIT_PAGE_SZ) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
	}
	uint8_t slot[AUDIT_SLOT_SZ];

	slot_encode(slot, e, evt, au.next_seq);
	entelea_result_t r = slot_commit(au.active_page, au.next_slot, slot);

	au.next_seq++; /* Failure also burns slot and seq (no rewrite) */
	au.next_slot++;
	if (au.next_slot == AUDIT_SLOTS) {
		au.next_slot = 0;
		au.active_page ^= 1U; /* Full: switch; erase before reuse */
	}
	if (r != ENTELEA_OK) {
		au.ready = false;
	}
	return r;
}

int32_t
entelea_storage_audit_read(entelea_storage_audit_entry_t *out, size_t max)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (!au.ready) {
		return ENTELEA_E_STATE;
	}
	/* Collect both pages (at most 128 transiently), then take the newest
	 * max entries by seq. Uncommitted and foreign slots are invisible. The
	 * buffer is static (a 4KB thread stack overflowed in real-device
	 * tests); the read path is low-frequency, single consumer. */
	static entelea_storage_audit_entry_t all[AUDIT_SLOTS * AUDIT_PAGES];
	size_t n = 0;

	for (uint8_t p = 0; p < AUDIT_PAGES; p++) {
		for (uint8_t i = 0; i < AUDIT_SLOTS; i++) {
			uint8_t slot[AUDIT_SLOT_SZ];

			if (slot_read(p, i, slot) != ENTELEA_OK) {
				au.ready = false;
				return ENTELEA_E_IO;
			}
			enum slot_kind kind = slot_kind(slot);

			if (kind == SLOT_CORRUPT) {
				au.ready = false;
				return ENTELEA_E_IO;
			}
			if (kind == SLOT_VALID) {
				slot_decode(slot, &all[n]);
				n++;
			}
		}
	}
	/* Insertion sort (seq ascending; n <= 128, low frequency). */
	for (size_t i = 1; i < n; i++) {
		entelea_storage_audit_entry_t k = all[i];
		size_t j = i;

		while (j > 0 && all[j - 1].seq > k.seq) {
			all[j] = all[j - 1];
			j--;
		}
		all[j] = k;
	}
	size_t take = (n > max) ? max : n;

	memcpy(out, &all[n - take], take * sizeof(out[0]));
	return (int32_t) take;
}
