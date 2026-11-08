/* SPDX-License-Identifier: Apache-2.0 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <entelea/app.h>
#include <ts/common.h>
#include <ts/err.h>

#if TS_E_PARAM != -1 || TS_E_PENDING != -13
#error "Legacy error macros must preserve preprocessor values"
#endif

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		exit(1); \
	} \
} while (0)

typedef struct {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	uint32_t buf_len;
} legacy_sha_layout_t;

typedef struct {
	const uint8_t *p;
	size_t rem;
} legacy_reader_layout_t;

_Static_assert(sizeof(ts_sha256_ctx_t) == sizeof(legacy_sha_layout_t), "SHA ABI size");
_Static_assert(offsetof(ts_sha256_ctx_t, buf_len) == offsetof(legacy_sha_layout_t, buf_len),
	"SHA ABI offsets");
_Static_assert(sizeof(ts_cbor_rd_t) == sizeof(legacy_reader_layout_t), "CBOR ABI size");
_Static_assert(offsetof(ts_cbor_rd_t, rem) == offsetof(legacy_reader_layout_t, rem),
	"CBOR ABI offsets");

static void digest_matches(const uint8_t out[32], const char *expected)
{
	const char hex[] = "0123456789abcdef";
	char actual[65];

	for (size_t i = 0; i < 32; i++) {
		actual[i * 2] = hex[out[i] >> 4];
		actual[i * 2 + 1] = hex[out[i] & 15];
	}
	actual[64] = '\0';
	CHECK(strcmp(actual, expected) == 0);
}

static void test_sha(void)
{
	uint8_t out[32];
	entelea_sha256_ctx_t ctx;

	CHECK(entelea_sha256(NULL, 0, out) == ENTELEA_OK);
	digest_matches(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(entelea_sha256((const uint8_t *)"abc", 3, out) == ENTELEA_OK);
	digest_matches(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char multi[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

	CHECK(entelea_sha256((const uint8_t *)multi, strlen(multi), out) == ENTELEA_OK);
	digest_matches(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(entelea_sha256_init(&ctx) == ENTELEA_OK);
	uint8_t block[1000];

	memset(block, 'a', sizeof(block));
	for (size_t i = 0; i < 1000; i++) {
		CHECK(entelea_sha256_update(&ctx, block, sizeof(block)) == ENTELEA_OK);
	}
	CHECK(entelea_sha256_final(&ctx, out) == ENTELEA_OK);
	digest_matches(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	CHECK(entelea_sha256_update(&ctx, block, 1) == ENTELEA_E_RANGE);
	CHECK(entelea_sha256_final(&ctx, out) == ENTELEA_E_STATE);
	CHECK(entelea_sha256_init(&ctx) == ENTELEA_OK);
	entelea_sha256_ctx_t before = ctx;

	CHECK(entelea_sha256_update(&ctx, NULL, 1) == ENTELEA_E_PARAM);
	CHECK(memcmp(&ctx, &before, sizeof(ctx)) == 0);
	ctx.len = UINT64_MAX / 8;
	before = ctx;
	CHECK(entelea_sha256_update(&ctx, block, 1) == ENTELEA_E_RANGE);
	CHECK(memcmp(&ctx, &before, sizeof(ctx)) == 0);
	CHECK(entelea_sha256_init(NULL) == ENTELEA_E_PARAM);
	CHECK(entelea_sha256_final(NULL, out) == ENTELEA_E_PARAM);
	CHECK(entelea_sha256(block, 1, NULL) == ENTELEA_E_PARAM);
	ts_sha256_ctx_t legacy;

	ts_sha256_init(&legacy);
	ts_sha256_update(&legacy, (const uint8_t *)"a", 1);
	ts_sha256_update(&legacy, (const uint8_t *)"bc", 2);
	ts_sha256_final(&legacy, out);
	digest_matches(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void test_cbor(void)
{
	uint8_t buf[128];
	size_t pos = 0;
	const uint8_t expected[] = {0xa1, 0x61, 'x', 0x82, 0x18, 0x18, 0xf5};

	CHECK(entelea_cbor_put_map(buf, sizeof(buf), &pos, 1));
	CHECK(entelea_cbor_put_tstr(buf, sizeof(buf), &pos, "x"));
	CHECK(entelea_cbor_put_array(buf, sizeof(buf), &pos, 2));
	CHECK(entelea_cbor_put_uint(buf, sizeof(buf), &pos, 24));
	CHECK(entelea_cbor_put_bool(buf, sizeof(buf), &pos, true));
	CHECK(pos == sizeof(expected) && memcmp(buf, expected, pos) == 0);
	entelea_cbor_reader_t r;
	uint32_t n;
	uint64_t u;
	bool b;
	char text[2];

	CHECK(entelea_cbor_init(&r, buf, pos));
	CHECK(entelea_cbor_map_open(&r, &n) && n == 1);
	CHECK(entelea_cbor_tstr(&r, text, sizeof(text)) && strcmp(text, "x") == 0);
	CHECK(entelea_cbor_array_open(&r, &n) && n == 2);
	CHECK(entelea_cbor_uint(&r, &u) && u == 24);
	CHECK(entelea_cbor_bool(&r, &b) && b && r.rem == 0);
	const uint64_t values[] = {0, 23, 24, 255, 256, 65535, 65536, UINT32_MAX,
				  (uint64_t)UINT32_MAX + 1, UINT64_MAX};
	const size_t sizes[] = {1, 1, 2, 2, 3, 3, 5, 5, 9, 9};

	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		pos = 0;
		CHECK(entelea_cbor_put_uint(buf, sizeof(buf), &pos, values[i]));
		CHECK(pos == sizes[i]);
		CHECK(entelea_cbor_init(&r, buf, pos));
		CHECK(entelea_cbor_uint(&r, &u) && u == values[i] && r.rem == 0);
		for (size_t cut = 0; cut < pos; cut++) {
			CHECK(entelea_cbor_init(&r, buf, cut));
			entelea_cbor_reader_t saved = r;
			u = 123;
			CHECK(!entelea_cbor_uint(&r, &u));
			CHECK(r.p == saved.p && r.rem == saved.rem && u == 123);
		}
	}
	const int64_t ints[] = {INT64_MIN, -25, -1, 0, INT64_MAX};

	for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
		int64_t value;

		pos = 0;
		CHECK(entelea_cbor_put_int(buf, sizeof(buf), &pos, ints[i]));
		CHECK(entelea_cbor_init(&r, buf, pos));
		CHECK(entelea_cbor_int(&r, &value) && value == ints[i]);
	}
	pos = 0;
	CHECK(entelea_cbor_put_uint(buf, sizeof(buf), &pos, UINT64_MAX));
	CHECK(entelea_cbor_init(&r, buf, pos));
	int64_t signed_value = 9;

	CHECK(!entelea_cbor_int(&r, &signed_value) && signed_value == 9 && r.rem == pos);
	const uint8_t rejected[][3] = {{0x9f, 0xff, 0}, {0xc0, 0, 0}, {0xf9, 0, 0}, {0xf8, 20, 0},
				     {0xe1, 0, 0}, {0x1c, 0, 0}, {0xf7, 0, 0}};

	for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
		CHECK(entelea_cbor_init(&r, rejected[i], sizeof(rejected[i])));
		CHECK(!entelea_cbor_skip(&r, 0) && r.rem == sizeof(rejected[i]));
	}
	const uint8_t deep[] = {0x81, 0x81, 0x81, 0x81, 0x81, 0};
	CHECK(entelea_cbor_init(&r, deep, sizeof(deep)));
	CHECK(!entelea_cbor_skip(&r, 0) && r.rem == sizeof(deep));
	CHECK(!entelea_cbor_skip(&r, -1));
	const uint8_t bounded[] = {0x81, 0x81, 0x81, 0x81, 0xf6};
	CHECK(entelea_cbor_init(&r, bounded, sizeof(bounded)));
	CHECK(entelea_cbor_skip(&r, 0) && r.rem == 0);
	const uint8_t oversized_map[] = {0xb1};
	const uint8_t oversized_array[] = {0x98, 65};
	const uint8_t oversized_bstr[] = {0x5b, 0, 0, 0, 1, 0, 0, 0, 0};
	const uint8_t oversized_skip[] = {0x59, 0x10, 0x01};
	CHECK(entelea_cbor_init(&r, oversized_map, sizeof(oversized_map)));
	CHECK(!entelea_cbor_map_open(&r, &n) && r.rem == sizeof(oversized_map));
	CHECK(entelea_cbor_init(&r, oversized_array, sizeof(oversized_array)));
	CHECK(!entelea_cbor_array_open(&r, &n) && r.rem == sizeof(oversized_array));
	CHECK(entelea_cbor_init(&r, oversized_bstr, sizeof(oversized_bstr)));
	const uint8_t *missing_ref = NULL;
	n = 13;
	CHECK(!entelea_cbor_bstr_ref(&r, &missing_ref, &n) && missing_ref == NULL && n == 13);
	CHECK(entelea_cbor_init(&r, oversized_skip, sizeof(oversized_skip)));
	CHECK(!entelea_cbor_skip(&r, 0) && r.rem == sizeof(oversized_skip));
	const uint8_t short_string[] = {0x63, 'a'};
	CHECK(entelea_cbor_init(&r, short_string, sizeof(short_string)));
	strcpy(text, "z");
	CHECK(!entelea_cbor_tstr(&r, text, sizeof(text)) && text[0] == 'z');
	pos = 0;
	CHECK(entelea_cbor_put_bstr(buf, sizeof(buf), &pos, (const uint8_t *)"hi", 2));
	const uint8_t *ref;
	CHECK(entelea_cbor_init(&r, buf, pos));
	CHECK(entelea_cbor_bstr_ref(&r, &ref, &n) && n == 2 && ref == buf + 1);
	memset(buf, 0x55, sizeof(buf));
	uint8_t before[sizeof(buf)];
	memcpy(before, buf, sizeof(buf));
	pos = sizeof(buf) - 1;
	CHECK(!entelea_cbor_put_tstr(buf, sizeof(buf), &pos, "a"));
	CHECK(pos == sizeof(buf) - 1 && memcmp(buf, before, sizeof(buf)) == 0);
	pos = SIZE_MAX;
	CHECK(!entelea_cbor_put_bool(buf, sizeof(buf), &pos, false) && pos == SIZE_MAX);
	pos = 0;
	CHECK(!entelea_cbor_put_tstrn(buf, sizeof(buf), &pos, "a", SIZE_MAX));
	CHECK(!entelea_cbor_init(NULL, buf, 1));
	CHECK(!entelea_cbor_init(&r, NULL, 1));
	CHECK(!entelea_cbor_uint(NULL, &u));
	CHECK(entelea_cbor_init(&r, NULL, 0));
	CHECK(!entelea_cbor_uint(&r, &u));
	CHECK(entelea_cbor_put_tstrn(buf, sizeof(buf), &pos, NULL, 0));
	pos = 0;
	CHECK(ts_cbor_put_map(buf, sizeof(buf), &pos, 1));
	CHECK(ts_cbor_put_tstr(buf, sizeof(buf), &pos, "x"));
	CHECK(ts_cbor_put_array(buf, sizeof(buf), &pos, 2));
	CHECK(ts_cbor_put_uint(buf, sizeof(buf), &pos, 24));
	CHECK(ts_cbor_put_bool(buf, sizeof(buf), &pos, true));
	CHECK(pos == sizeof(expected) && memcmp(buf, expected, pos) == 0);
}

typedef struct {
	uint64_t now;
	entelea_result_t result;
} fake_clock_t;

static entelea_result_t fake_read(void *user, uint64_t *out)
{
	fake_clock_t *clock = user;

	*out = clock->now;
	return clock->result;
}

static void test_contracts(void)
{
	fake_clock_t fake = {.now = 0, .result = ENTELEA_OK};
	entelea_clock_t clock = {0};
	entelea_instant_t now = {.ms = 123}, deadline;

	CHECK(entelea_clock_now(&clock, &now) == ENTELEA_E_UNINIT && now.ms == 123);
	CHECK(entelea_clock_init(&clock, fake_read, &fake, 1) == ENTELEA_OK);
	CHECK(entelea_clock_now(&clock, &now) == ENTELEA_OK && now.valid && now.ms == 0);
	CHECK(entelea_deadline_after(now, 10, &deadline) == ENTELEA_OK);
	bool expired = true;
	CHECK(entelea_deadline_expired(now, deadline, &expired) == ENTELEA_OK && !expired);
	fake.now = 10;
	CHECK(entelea_clock_now(&clock, &now) == ENTELEA_OK);
	CHECK(entelea_deadline_expired(now, deadline, &expired) == ENTELEA_OK && expired);
	fake.now = 9;
	CHECK(entelea_clock_now(&clock, &now) == ENTELEA_E_STATE && now.ms == 10);
	fake.result = ENTELEA_E_IO;
	CHECK(entelea_clock_now(&clock, &now) == ENTELEA_E_IO && now.ms == 10);
	fake.result = ENTELEA_PENDING;
	CHECK(entelea_clock_now(&clock, &now) == ENTELEA_PENDING && now.ms == 10);
	now.ms = UINT64_MAX;
	deadline.ms = 7;
	CHECK(entelea_deadline_after(now, 1, &deadline) == ENTELEA_E_RANGE && deadline.ms == 7);
	now.valid = false;
	CHECK(entelea_deadline_after(now, 0, &deadline) == ENTELEA_E_PARAM);
	now.valid = true;
	deadline.clock_id = 2;
	CHECK(entelea_deadline_expired(now, deadline, &expired) == ENTELEA_E_PARAM);
	CHECK(entelea_clock_init(NULL, fake_read, &fake, 1) == ENTELEA_E_PARAM);
	CHECK(entelea_clock_init(&clock, NULL, &fake, 1) == ENTELEA_E_PARAM);
	CHECK(entelea_clock_init(&clock, fake_read, &fake, 0) == ENTELEA_E_PARAM);
	CHECK(TS_OK == 0 && TS_E_RANGE == -12 && TS_E_PENDING == -13);
	CHECK(TS_FAIL_BOOT(7) == 0x10007U);
	entelea_app_request_t wasm = {
		.runtime = ENTELEA_RUNTIME_WASM, .payload_version = 1,
		.budget = {.memory_bytes = 1024, .execution_limit = 100,
			   .execution_unit = ENTELEA_METER_RUNTIME_STEPS}
	};
	entelea_app_request_t lua = wasm;
	lua.runtime = ENTELEA_RUNTIME_LUA;
	lua.budget.execution_unit = ENTELEA_METER_CPU_US;
	CHECK(wasm.budget.execution_unit != lua.budget.execution_unit);
	CHECK(wasm.principal == NULL && lua.principal == NULL);
	entelea_call_receipt_t cancelled = {
		.state = ENTELEA_CALL_CANCELLED, .effect = ENTELEA_EFFECT_UNKNOWN
	};
	CHECK(cancelled.effect == ENTELEA_EFFECT_UNKNOWN);
}

int main(void)
{
	test_sha();
	test_cbor();
	test_contracts();
	puts("PASS: SHA/CBOR/compat/time/App representation contracts");
	return 0;
}
