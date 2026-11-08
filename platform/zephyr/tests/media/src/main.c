/* SPDX-License-Identifier: Apache-2.0 */
/* R09 媒体接口测试：生命周期/权限/取消不复活/失败显式/独立可选。 */
#include <string.h>
#include <zephyr/ztest.h>
#include <entelea/media.h>
#include <entelea/principal.h>

static void *media_setup(void)
{
	entelea_media_test_reset();
	entelea_perm_test_reset();
	return NULL;
}

ZTEST(framework_media, test_lifecycle_and_permission_gate)
{
	entelea_media_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_perm_test_reset();
	entelea_media_frame_t f;
	entelea_perm_table_t t;

	/* 未绑定=UNINIT。 */
	zassert_equal(entelea_media_capture_start((entelea_principal_t){._handle = 0, .app_id = 0}),
		      ENTELEA_E_UNINIT);
	zassert_equal(entelea_media_poll(&f), ENTELEA_E_UNINIT);
	zassert_equal(entelea_media_state(), ENTELEA_MEDIA_IDLE);

	zassert_equal(entelea_media_bind(entelea_media_fake_backend, NULL),
		      ENTELEA_OK);

	/* 未授权采集拒（av:read 门）。 */
	entelea_perm_table_init(&t);
	entelea_principal_t poor;

	zassert_equal(entelea_principal_bind(&poor, 2, &t), ENTELEA_OK);
	zassert_equal(entelea_media_capture_start(poor), ENTELEA_E_PERM);
	zassert_equal(entelea_media_state(), ENTELEA_MEDIA_IDLE,
		      "拒绝路径零启动副作用");

	/* 授权启动+取帧。 */
	zassert_equal(entelea_perm_parse("av:read:0", &t), ENTELEA_OK);
	entelea_principal_t ok_ctx;

	zassert_equal(entelea_principal_bind(&ok_ctx, 1, &t), ENTELEA_OK);
	static const uint8_t pat[4] = {0xDE, 0xAD, 0xBE, 0xEF};

	entelea_media_fake_set_frames(pat, 4, 2);
	zassert_equal(entelea_media_capture_start(ok_ctx), ENTELEA_OK);
	zassert_equal(entelea_media_state(), ENTELEA_MEDIA_CAPTURING);
	zassert_equal(entelea_media_fake_start_calls(), 1, "正向控制");
	zassert_equal(entelea_media_poll(&f), ENTELEA_OK);
	zassert_equal(f.len, 4);
	zassert_ok(memcmp(f.bytes, pat, 4));
	zassert_equal(entelea_media_poll(&f), ENTELEA_OK);
	/* 帧耗尽=E_BUSY（非失败）。 */
	zassert_equal(entelea_media_poll(&f), ENTELEA_E_BUSY, "无帧非失败");

	/* 停止幂等；停止后 poll 不复活。 */
	entelea_media_capture_stop();
	entelea_media_capture_stop();
	zassert_equal(entelea_media_state(), ENTELEA_MEDIA_IDLE);
	zassert_equal(entelea_media_poll(&f), ENTELEA_E_STATE, "停止不复活");

	/* 重复启动拒。 */
	entelea_media_fake_set_frames(pat, 4, 1);
	zassert_equal(entelea_media_capture_start(ok_ctx), ENTELEA_OK);
	zassert_equal(entelea_media_capture_start(ok_ctx), ENTELEA_E_BUSY,
		      "重复启动显式拒");
	entelea_media_capture_stop();
}

ZTEST(framework_media, test_backend_failure_explicit)
{
	entelea_media_test_reset();
	entelea_perm_test_reset();
	static const uint8_t pat[2] = {1, 2};

	entelea_media_bind(entelea_media_fake_backend, NULL);
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("av:read:0", &t), ENTELEA_OK);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 3, &t), ENTELEA_OK);

	/* start 失败显式（不静默成功/不置 CAPTURING）。 */
	entelea_media_fake_fail_next();
	entelea_media_fake_set_frames(pat, 2, 1);
	zassert_equal(entelea_media_capture_start(ctx), ENTELEA_E_IO);
	zassert_equal(entelea_media_state(), ENTELEA_MEDIA_IDLE);

	/* poll 失败 → FAILED 态（后端失败不报告成功）。 */
	entelea_media_fake_set_frames(pat, 2, 2);
	zassert_equal(entelea_media_capture_start(ctx), ENTELEA_OK);
	entelea_media_frame_t f;

	entelea_media_fake_fail_next();
	zassert_equal(entelea_media_poll(&f), ENTELEA_E_IO);
	zassert_equal(entelea_media_state(), ENTELEA_MEDIA_FAILED,
		      "失败态可观测");
	/* FAILED 后 poll=E_STATE（不复活——须重启采集）。 */
	zassert_equal(entelea_media_poll(&f), ENTELEA_E_STATE);
	/* 重启恢复。 */
	entelea_media_fake_set_frames(pat, 2, 1);
	zassert_equal(entelea_media_capture_start(ctx), ENTELEA_OK);
	zassert_equal(entelea_media_poll(&f), ENTELEA_OK);
	entelea_media_capture_stop();

	/* 绑定面负测：缺 ops 拒。 */
	entelea_media_backend_t broken = entelea_media_fake_backend;

	broken.poll = NULL;
	zassert_equal(entelea_media_bind(broken, NULL), ENTELEA_E_PARAM);
	zassert_not_equal(entelea_media_capture_start(ctx), ENTELEA_E_UNINIT,
			  "坏绑定不改既有绑定");
	entelea_media_capture_stop();
}

ZTEST_SUITE(framework_media, NULL, media_setup, NULL, NULL, NULL);
