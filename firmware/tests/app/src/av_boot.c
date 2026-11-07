// SPDX-License-Identifier: Apache-2.0
#include <string.h>

#include <ts/appmgr.h>
#include <ts/hal.h>
#include <ts/store.h>
#include <ts/tsap.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "../../../module/tessera/src/store/internal.h"
#include "av_boot.h"
#include "boot_pkg.h"

static bool testing_boot;
static ts_res_t binding_result;
static unsigned int binding_calls;
static unsigned int start_calls;

ts_res_t __wrap_ts_av_config_bind(uint16_t app_id, ts_av_fmt_t fmt, uint16_t w, uint16_t h)
{
	if (!testing_boot) {
		return __real_ts_av_config_bind(app_id, fmt, w, h);
	}
	zassert_equal(app_id, 1);
	zassert_equal(fmt, TS_AV_FMT_RGB565);
	zassert_equal(w, 16);
	zassert_equal(h, 16);
	zassert_equal(start_calls, 0, "App started before AV binding");
	binding_calls++;
	return binding_result;
}

ts_res_t __wrap_ts_appmgr_app_start(uint16_t app_id, const uint8_t *wasm, uint32_t len,
				    const char *caps)
{
	if (!testing_boot) {
		return __real_ts_appmgr_app_start(app_id, wasm, len, caps);
	}
	zassert_equal(binding_calls, 1);
	zassert_equal(binding_result, TS_OK);
	start_calls++;
	return TS_OK;
}

static void prepare_boot(void *fixture)
{
	/* Boot persistence fixture only; signature/install coverage stays in the existing tests. */
	static const uint8_t av_fields[] = {
		0x66, 'a',  'v', '_', 'f', 'm', 't',  0x66, 'r', 'g', 'b', '5', '6',
		'5',  0x64, 'a', 'v', '_', 'w', 0x10, 0x64, 'a', 'v', '_', 'h', 0x10,
	};
	uint8_t package[sizeof(boot_pkg) + sizeof(av_fields)];
	uint32_t manifest_len = tsap_be32(boot_pkg + 6);
	uint32_t wasm_len = tsap_be32(boot_pkg + 10);
	ts_appmgr_meta_t meta;

	ARG_UNUSED(fixture);
	ts_appmgr_app_test_reset();
	ts_appmgr_test_reset();
	ts_store_test_reset();
	memcpy(package, boot_pkg, TSAP_CONTENT_OFF + manifest_len);
	zassert_true(package[TSAP_CONTENT_OFF] >= 0xa0 && package[TSAP_CONTENT_OFF] <= 0xb4);
	package[TSAP_CONTENT_OFF] += 3;
	memcpy(package + TSAP_CONTENT_OFF + manifest_len, av_fields, sizeof(av_fields));
	memcpy(package + TSAP_CONTENT_OFF + manifest_len + sizeof(av_fields),
	       boot_pkg + TSAP_CONTENT_OFF + manifest_len,
	       sizeof(boot_pkg) - TSAP_CONTENT_OFF - manifest_len);
	manifest_len += sizeof(av_fields);
	sys_put_be32(manifest_len, package + 6);
	ts_sha256(package + TSAP_CONTENT_OFF, manifest_len + wasm_len, package + TSAP_DIGEST_OFF);
	zassert_equal(ts_store_slot_write(0, 0, package, sizeof(package)), TS_OK);
	ts_appmgr_meta_default(&meta);
	zassert_equal(ts_appmgr_meta_write(&meta), TS_OK);
	binding_calls = 0;
	start_calls = 0;
	binding_result = TS_OK;
	testing_boot = true;
}

static void finish_boot(void *fixture)
{
	ARG_UNUSED(fixture);
	testing_boot = false;
	ts_appmgr_test_reset();
	ts_store_test_reset();
}

ZTEST(av_boot, test_binding_failure_does_not_start_app)
{
	binding_result = TS_E_IO;
	zassert_equal(ts_appmgr_boot_start(), TS_E_IO);
	zassert_equal(binding_calls, 1);
	zassert_equal(start_calls, 0);
	zassert_false(ts_appmgr_app_running());
}

ZTEST(av_boot, test_binding_precedes_app_start)
{
	zassert_equal(ts_appmgr_boot_start(), TS_OK);
	zassert_equal(binding_calls, 1);
	zassert_equal(start_calls, 1);
}

ZTEST_SUITE(av_boot, NULL, NULL, prepare_boot, finish_boot, NULL);
