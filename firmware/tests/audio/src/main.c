// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <ts/hal.h>
#include <zephyr/ztest.h>

#define FRAME_BYTES 640U

static int init_result;
static int capture_result;
static int write_result;
static unsigned int init_calls;
static unsigned int capture_calls;
static unsigned int write_calls;
static bool allowed;

ts_res_t ts_perm_check(ts_ctx_t ctx, ts_perm_class_t cls, ts_perm_op_t op, uint8_t inst)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(cls);
	ARG_UNUSED(op);
	ARG_UNUSED(inst);
	return allowed ? TS_OK : TS_E_PERM;
}

static int fake_init(void)
{
	init_calls++;
	return init_result;
}

static int fake_capture(uint8_t *buf, uint32_t len)
{
	capture_calls++;
	zassert_equal(len, FRAME_BYTES);
	memset(buf, 0x5a, len);
	return capture_result;
}

#if defined(CONFIG_TS_DRV_PDM)
int ts_drv_pdm_init(void)
{
	return fake_init();
}

int ts_drv_pdm_capture(uint8_t *buf, uint32_t len)
{
	return fake_capture(buf, len);
}
#else
int ts_drv_es8311_init(void)
{
	return fake_init();
}

int ts_drv_es8311_capture(uint8_t *buf, uint32_t len)
{
	return fake_capture(buf, len);
}

int ts_drv_es8311_write(const uint8_t *buf, uint32_t len)
{
	zassert_not_null(buf);
	zassert_equal(len, FRAME_BYTES);
	write_calls++;
	return write_result;
}
#endif

static void reset_test(void *fixture)
{
	ARG_UNUSED(fixture);
	init_result = 0;
	capture_result = FRAME_BYTES;
	write_result = FRAME_BYTES;
	init_calls = 0;
	capture_calls = 0;
	write_calls = 0;
	allowed = true;
	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_PCM16, 16, 0), TS_OK);
}

ZTEST(audio_contract, test_capture_full_frame)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t buf[FRAME_BYTES + 1];
	uint32_t len = 17;

	memset(buf, 0, sizeof(buf));
	buf[FRAME_BYTES] = 0xa5;
	zassert_equal(ts_av_capture(ctx, buf, sizeof(buf), &len), TS_OK);
	zassert_equal(len, FRAME_BYTES);
	zassert_equal(buf[0], 0x5a);
	zassert_equal(buf[FRAME_BYTES - 1], 0x5a);
	zassert_equal(buf[FRAME_BYTES], 0xa5);
	zassert_equal(init_calls, 1);
	zassert_equal(capture_calls, 1);
}

ZTEST(audio_contract, test_capture_rejects_bad_backend_results)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t buf[FRAME_BYTES];
	const int results[] = {-2, -1, 0, 1, FRAME_BYTES - 1, FRAME_BYTES + 1};

	for (size_t i = 0; i < ARRAY_SIZE(results); i++) {
		uint32_t len = 23;

		capture_result = results[i];
		zassert_equal(ts_av_capture(ctx, buf, sizeof(buf), &len), TS_E_IO);
		zassert_equal(len, 23);
	}
}

ZTEST(audio_contract, test_capture_init_failure_and_gates)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t buf[FRAME_BYTES];
	uint32_t len = 31;

	zassert_equal(ts_av_capture(ctx, buf, FRAME_BYTES - 1, &len), TS_E_RANGE);
	zassert_equal(init_calls, 0);
	allowed = false;
	zassert_equal(ts_av_capture(ctx, buf, sizeof(buf), &len), TS_E_PERM);
	zassert_equal(init_calls, 0);
	allowed = true;
	ctx.app_id = 43;
	zassert_equal(ts_av_capture(ctx, buf, sizeof(buf), &len), TS_E_STATE);
	zassert_equal(init_calls, 0);
	ctx.app_id = 42;
	init_result = -1;
	zassert_equal(ts_av_capture(ctx, buf, sizeof(buf), &len), TS_E_IO);
	zassert_equal(capture_calls, 0);
	zassert_equal(len, 31);
}

#if defined(CONFIG_TS_DRV_ES8311)
ZTEST(audio_contract, test_playback_rejects_partial_backend_write)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t buf[FRAME_BYTES] = {0};
	const int results[] = {-2, 0, 1, FRAME_BYTES - 1, FRAME_BYTES + 1};

	for (size_t i = 0; i < ARRAY_SIZE(results); i++) {
		write_result = results[i];
		zassert_equal(ts_av_write(ctx, buf, sizeof(buf)), TS_E_IO);
	}
	write_result = FRAME_BYTES;
	zassert_equal(ts_av_write(ctx, buf, sizeof(buf)), TS_OK);
	zassert_equal(write_calls, ARRAY_SIZE(results) + 1);
}
#endif

ZTEST_SUITE(audio_contract, NULL, NULL, reset_test, NULL, NULL);
