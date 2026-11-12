/* SPDX-License-Identifier: Apache-2.0 */
/*
 * APP business-state kv (migrated from the old store/kv.c; R03 contract in
 * this directory's contract.md): one bounded blob per APP (<=250B), kept
 * across upgrades and rollbacks by app_id hash ownership.
 * Layout: 8KB = two pages x 4KB x 15 slots x 264B (14B header + <=250B).
 * The page tail holds the CRC16 of each newly written slot and an independent
 * commit tag; old slots remain readable but cannot be verified.
 * Append-only writes (re-saving an app creates a new slot; load takes the
 * valid slot with the largest seq across both pages).
 * RF1 (review P1-5/P1-6): body, integrity side band, original magic and side
 * band commit tag are written in that order. Uncommitted slots are skipped;
 * committed but corrupt slots are reported explicitly. A failed write burns
 * that slot (NOR does not rewrite programmed slots). Compaction first reads
 * everything into a buffer and confirms that the active page holds a complete
 * copy of every retained record before erasing the other page. Copies keep
 * their original seq and are written in ascending seq order. If a partial
 * commit tag is written, recovery rejects rather than rolling back.
 * Capacity: at most 14 apps are retained across compaction (beyond that, the
 * least recently written app is dropped; bounded semantics).
 */
#include <string.h>
#include <entelea/storage.h>
#include "internal.h"

#define KV_MAGIC 0x54534B31U /* "TSK1" */
#define KV_PAGE_SZ 4096U
#define KV_SLOT_SZ 264U
#define KV_SLOTS (KV_PAGE_SZ / KV_SLOT_SZ) /* 15 */
#define KV_PAGES 2U
#define KV_KEEP_MAX (KV_SLOTS - 1)

#define KV_OFF_MAGIC 0
#define KV_OFF_HASH 4
#define KV_OFF_SEQ 8
#define KV_OFF_LEN 12
#define KV_OFF_DATA 14
#define KV_TAG_BASE (KV_SLOT_SZ * KV_SLOTS)
#define KV_TAG_SIZE 8U
#define KV_TAG_MAGIC 0xA56CU
#define KV_TAG_COMMIT 0x3143564BU

_Static_assert(KV_TAG_BASE + KV_SLOTS * KV_TAG_SIZE <= KV_PAGE_SZ,
	       "KV integrity tags must fit in the page tail");

struct kv_slot {
	uint8_t data[KV_SLOT_SZ];
	uint8_t tag[KV_TAG_SIZE];
};

enum slot_kind {
	SLOT_INCOMPLETE,
	SLOT_VALID,
	SLOT_CORRUPT,
};

static struct {
	bool ready;
	uint8_t active_page;
	uint8_t next_slot; /* KV_SLOTS = page full: compact before next save */
	uint32_t next_seq; /* global monotonic (from 1; 0/0xFFFFFFFF unused) */
} kv;

static entelea_result_t
slot_read(uint8_t page, uint8_t i, struct kv_slot *s)
{
	if (entelea_store_backend.read(
		    ENTELEA_PART_KV, page * KV_PAGE_SZ + i * KV_SLOT_SZ,
		    s->data, sizeof(s->data)) != ENTELEA_OK ||
	    entelea_store_backend.read(ENTELEA_PART_KV,
				       page * KV_PAGE_SZ + KV_TAG_BASE +
					       i * KV_TAG_SIZE,
				       s->tag, sizeof(s->tag)) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

static bool
all_erased(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0xFFU) {
			return false;
		}
	}
	return true;
}

static enum slot_kind
slot_kind(const struct kv_slot *s)
{
	const uint32_t magic = entelea_get_le32(s->data + KV_OFF_MAGIC);
	const uint32_t seq = entelea_get_le32(s->data + KV_OFF_SEQ);
	const uint16_t len = entelea_get_le16(s->data + KV_OFF_LEN);
	const bool fields_valid = magic == KV_MAGIC && seq != 0 &&
				  seq != UINT32_MAX && len >= 1 &&
				  len <= ENTELEA_KV_BLOB_MAX;
	const bool legacy = all_erased(s->tag, sizeof(s->tag));
	const uint32_t commit = entelea_get_le32(s->tag + 4);

	if (legacy) {
		return fields_valid ? SLOT_VALID : SLOT_INCOMPLETE;
	}
	if (commit == UINT32_MAX) {
		return SLOT_INCOMPLETE;
	}
	if (commit != KV_TAG_COMMIT ||
	    entelea_get_le16(s->tag + 2) != KV_TAG_MAGIC || !fields_valid ||
	    entelea_get_le16(s->tag) !=
		    entelea_crc16(s->data + KV_OFF_HASH,
				  KV_OFF_DATA - KV_OFF_HASH + len)) {
		return SLOT_CORRUPT;
	}
	return SLOT_VALID;
}

static bool
slot_erased(const struct kv_slot *s)
{
	return all_erased(s->data, sizeof(s->data)) &&
	       all_erased(s->tag, sizeof(s->tag));
}

static entelea_result_t
slot_commit(uint8_t page, uint8_t i, const struct kv_slot *s)
{
	uint32_t off = page * KV_PAGE_SZ + i * KV_SLOT_SZ;
	uint32_t tag_off = page * KV_PAGE_SZ + KV_TAG_BASE + i * KV_TAG_SIZE;
	bool tagged = !all_erased(s->tag, sizeof(s->tag));
	struct kv_slot back;

	if (entelea_store_backend.write(
		    ENTELEA_PART_KV, off + KV_OFF_HASH, s->data + KV_OFF_HASH,
		    KV_SLOT_SZ - KV_OFF_HASH) != ENTELEA_OK ||
	    (tagged && entelea_store_backend.write(ENTELEA_PART_KV, tag_off,
						   s->tag, 4) != ENTELEA_OK) ||
	    entelea_store_backend.write(ENTELEA_PART_KV, off, s->data,
					KV_OFF_HASH) != ENTELEA_OK ||
	    (tagged &&
	     entelea_store_backend.write(ENTELEA_PART_KV, tag_off + 4,
					 s->tag + 4, 4) != ENTELEA_OK) ||
	    slot_read(page, i, &back) != ENTELEA_OK ||
	    memcmp(&back, s, sizeof(back)) != 0) {
		return ENTELEA_E_IO;
	}
	return ENTELEA_OK;
}

/* Page scan: max valid seq plus used-slot count (last non-erased slot + 1;
 * bad slots are not reused). */
static entelea_result_t
page_scan(uint8_t page, uint32_t *max_seq, uint8_t *used)
{
	struct kv_slot s;

	*max_seq = 0;
	*used = 0;
	for (uint8_t i = 0; i < KV_SLOTS; i++) {
		if (slot_read(page, i, &s) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
		enum slot_kind kind = slot_kind(&s);

		if (kind == SLOT_CORRUPT) {
			return ENTELEA_E_IO;
		}
		if (kind == SLOT_VALID &&
		    entelea_get_le32(s.data + KV_OFF_SEQ) > *max_seq) {
			*max_seq = entelea_get_le32(s.data + KV_OFF_SEQ);
		}
		if (!slot_erased(&s)) {
			*used = (uint8_t) (i + 1);
		}
	}
	return ENTELEA_OK;
}

static void
slot_encode(struct kv_slot *s, uint32_t hash, const uint8_t *data, uint16_t len,
	    uint32_t seq)
{
	memset(s, 0xFF, sizeof(*s));
	entelea_put_le32(s->data + KV_OFF_MAGIC, KV_MAGIC);
	entelea_put_le32(s->data + KV_OFF_HASH, hash);
	entelea_put_le32(s->data + KV_OFF_SEQ, seq);
	entelea_put_le16(s->data + KV_OFF_LEN, len);
	memcpy(s->data + KV_OFF_DATA, data, len);
	entelea_put_le16(s->tag,
			 entelea_crc16(s->data + KV_OFF_HASH,
				       KV_OFF_DATA - KV_OFF_HASH + len));
	entelea_put_le16(s->tag + 2, KV_TAG_MAGIC);
	entelea_put_le32(s->tag + 4, KV_TAG_COMMIT);
}

struct kv_ref {
	uint32_t hash;
	uint32_t seq;
	uint8_t page;
	uint8_t slot;
};

/* Latest valid slot per app across both pages (at most 14; beyond that,
 * the oldest seq is dropped). */
static entelea_result_t
collect_latest(struct kv_ref *refs, size_t *count)
{
	struct kv_slot s;
	size_t n = 0;

	for (uint8_t p = 0; p < KV_PAGES; p++) {
		for (uint8_t i = 0; i < KV_SLOTS; i++) {
			if (slot_read(p, i, &s) != ENTELEA_OK) {
				return ENTELEA_E_IO;
			}
			enum slot_kind kind = slot_kind(&s);

			if (kind == SLOT_CORRUPT) {
				return ENTELEA_E_IO;
			}
			if (kind != SLOT_VALID) {
				continue;
			}
			uint32_t hash = entelea_get_le32(s.data + KV_OFF_HASH);
			uint32_t seq = entelea_get_le32(s.data + KV_OFF_SEQ);
			size_t hit = n;

			for (size_t k = 0; k < n; k++) {
				if (refs[k].hash == hash) {
					hit = k;
					break;
				}
			}
			if (hit < n) {
				if (refs[hit].seq > seq) {
					continue;
				}
				if (refs[hit].seq == seq) {
					struct kv_slot previous;

					if (slot_read(refs[hit].page,
						      refs[hit].slot,
						      &previous) !=
						    ENTELEA_OK ||
					    memcmp(&previous, &s, sizeof(s)) !=
						    0) {
						return ENTELEA_E_IO;
					}
					continue;
				}
			} else if (n == KV_KEEP_MAX) {
				size_t oldest = 0;

				for (size_t k = 1; k < n; k++) {
					if (refs[k].seq < refs[oldest].seq) {
						oldest = k;
					}
				}
				if (refs[oldest].seq > seq) {
					continue;
				}
				hit = oldest;
			} else {
				n++;
			}
			refs[hit] = (struct kv_ref) { hash, seq, p, i };
		}
	}
	*count = n;
	return ENTELEA_OK;
}

/* Compaction on full page: read before erase, write before switch. Never
 * erase the only retained record on the target page. */
static entelea_result_t
compact(void)
{
	static struct kv_ref refs[KV_KEEP_MAX];
	static struct kv_slot buf[KV_KEEP_MAX];
	size_t n = 0;
	entelea_result_t r = collect_latest(refs, &n);

	if (r != ENTELEA_OK) {
		return r;
	}
	for (size_t i = 1; i < n; i++) { /* ascending seq: max lands last */
		struct kv_ref k = refs[i];
		size_t j = i;

		while (j > 0 && refs[j - 1].seq > k.seq) {
			refs[j] = refs[j - 1];
			j--;
		}
		refs[j] = k;
	}
	for (size_t i = 0; i < n; i++) {
		if (slot_read(refs[i].page, refs[i].slot, &buf[i]) !=
			    ENTELEA_OK ||
		    slot_kind(&buf[i]) != SLOT_VALID ||
		    entelea_get_le32(buf[i].data + KV_OFF_HASH) !=
			    refs[i].hash ||
		    entelea_get_le32(buf[i].data + KV_OFF_SEQ) != refs[i].seq) {
			return ENTELEA_E_IO;
		}
	}
	uint8_t target = kv.active_page ^ 1U;

	for (size_t i = 0; i < n; i++) {
		if (refs[i].page != target) {
			continue;
		}
		struct kv_slot source;
		bool found = false;

		for (uint8_t slot = 0; slot < KV_SLOTS; slot++) {
			if (slot_read(kv.active_page, slot, &source) !=
			    ENTELEA_OK) {
				return ENTELEA_E_IO;
			}
			enum slot_kind kind = slot_kind(&source);

			if (kind == SLOT_CORRUPT) {
				return ENTELEA_E_IO;
			}
			if (kind == SLOT_VALID &&
			    memcmp(&source, &buf[i], sizeof(source)) == 0) {
				found = true;
				break;
			}
		}
		if (!found) {
			return ENTELEA_E_STATE;
		}
	}

	if (entelea_store_backend.erase_off(ENTELEA_PART_KV,
					    target * KV_PAGE_SZ,
					    KV_PAGE_SZ) != ENTELEA_OK) {
		return ENTELEA_E_IO;
	}
	for (size_t i = 0; i < n; i++) {
		if (slot_commit(target, (uint8_t) i, &buf[i]) != ENTELEA_OK) {
			return ENTELEA_E_IO;
		}
	}
	kv.active_page = target;
	kv.next_slot = (uint8_t) n;
	return ENTELEA_OK;
}

/* Public API. The trust header is separate; kv is merged into storage.h. */
#include <entelea/kv.h>

int32_t
entelea_kv_load(uint32_t app_hash, uint8_t *out, uint16_t max)
{
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = entelea_kv_init();

	if (r != ENTELEA_OK) {
		return r;
	}
	struct kv_slot s;
	bool found = false;
	uint32_t best_seq = 0;
	uint8_t bp = 0;
	uint8_t bi = 0;

	for (uint8_t p = 0; p < KV_PAGES; p++) {
		for (uint8_t i = 0; i < KV_SLOTS; i++) {
			if (slot_read(p, i, &s) != ENTELEA_OK) {
				kv.ready = false;
				return ENTELEA_E_IO;
			}
			enum slot_kind kind = slot_kind(&s);

			if (kind == SLOT_CORRUPT) {
				kv.ready = false;
				return ENTELEA_E_IO;
			}
			if (kind != SLOT_VALID ||
			    entelea_get_le32(s.data + KV_OFF_HASH) !=
				    app_hash) {
				continue;
			}
			uint32_t seq = entelea_get_le32(s.data + KV_OFF_SEQ);

			if (found && seq == best_seq) {
				struct kv_slot previous;

				if (slot_read(bp, bi, &previous) !=
					    ENTELEA_OK ||
				    memcmp(&previous, &s, sizeof(s)) != 0) {
					kv.ready = false;
					return ENTELEA_E_IO;
				}
			}
			if (!found || seq > best_seq) {
				found = true;
				best_seq = seq;
				bp = p;
				bi = i;
			}
		}
	}
	if (!found) {
		return ENTELEA_E_NOTFOUND;
	}
	if (slot_read(bp, bi, &s) != ENTELEA_OK ||
	    slot_kind(&s) != SLOT_VALID ||
	    entelea_get_le32(s.data + KV_OFF_HASH) != app_hash ||
	    entelea_get_le32(s.data + KV_OFF_SEQ) != best_seq) {
		kv.ready = false;
		return ENTELEA_E_IO;
	}
	uint16_t len = entelea_get_le16(s.data + KV_OFF_LEN);

	if (len > max) {
		return ENTELEA_E_RANGE; /* Too small; reported as-is */
	}
	memcpy(out, s.data + KV_OFF_DATA, len);
	return (int32_t) len;
}

entelea_result_t
entelea_kv_save(uint32_t app_hash, const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0 || len > ENTELEA_KV_BLOB_MAX) {
		return ENTELEA_E_PARAM;
	}
	entelea_result_t r = entelea_kv_init();

	if (r != ENTELEA_OK) {
		return r;
	}
	if (kv.next_slot >= KV_SLOTS) {
		r = compact();
		if (r != ENTELEA_OK) {
			return r;
		}
	}
	if (kv.next_seq == UINT32_MAX) {
		return ENTELEA_E_STATE;
	}
	struct kv_slot s;

	slot_encode(&s, app_hash, data, len, kv.next_seq);
	r = slot_commit(kv.active_page, kv.next_slot, &s);
	kv.next_seq++; /* Failure also burns slot and seq (no rewrite) */
	kv.next_slot++;
	if (r != ENTELEA_OK) {
		kv.ready = false;
	}
	return r;
}

entelea_result_t
entelea_kv_init(void)
{
	if (kv.ready) {
		return ENTELEA_OK;
	}
	if (entelea_store_backend.size(ENTELEA_PART_KV) <
	    KV_PAGES * KV_PAGE_SZ) {
		return ENTELEA_E_STATE; /* Partition not built; degraded */
	}
	uint32_t sa;
	uint32_t sb;
	uint8_t ua;
	uint8_t ub;

	if (page_scan(0, &sa, &ua) != ENTELEA_OK ||
	    page_scan(1, &sb, &ub) != ENTELEA_OK) {
		return ENTELEA_E_IO; /* Not ready: rescan next time */
	}
	/* Active page = the one with the largest valid seq. On a tie
	 * (compaction just finished, or both empty), take the page with
	 * fewer used slots. */
	kv.active_page = (sb > sa || (sb == sa && ub < ua)) ? 1U : 0U;
	kv.next_slot = kv.active_page ? ub : ua;
	kv.next_seq = ((sa > sb) ? sa : sb) + 1U;
	kv.ready = true;
	return ENTELEA_OK;
}

#ifdef ENTELEA_TEST
void
entelea_kv_test_reset(void)
{
	memset(&kv, 0, sizeof(kv));
}
#endif
