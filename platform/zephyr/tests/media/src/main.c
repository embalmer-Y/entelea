/* SPDX-License-Identifier: Apache-2.0 */
/* R09 媒体接口测试：生命周期/权限/取消不复活/失败显式/按类独立可选。 */
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

/* 假后端按类绑定：user=类号（fake 内部按类独立状态）。 */
static entelea_result_t bind_fake(entelea_media_class_t cls)
{
	return entelea_media_bind(cls, entelea_media_fake_backend,
				  (void *)(uintptr_t)cls);
}

ZTEST(framework_media, test_lifecycle_and_permission_gate)
{
	entelea_media_test_reset(); /* 用例序不保证：自含净基线 */
	entelea_perm_test_reset();
	entelea_media_frame_t f;
	entelea_perm_table_t t;

	/* 未绑定=UNINIT；未绑定类状态=IDLE（不伪装可用）。 */
	zassert_equal(
		entelea_media_capture_start(ENTELEA_MEDIA_PCM,
					    (entelea_principal_t){._handle = 0,
								  .app_id = 0}),
		ENTELEA_E_UNINIT);
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f),
		      ENTELEA_E_UNINIT);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_IDLE);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_IMAGE),
		      ENTELEA_MEDIA_IDLE, "未绑定类不伪装可用");

	zassert_equal(bind_fake(ENTELEA_MEDIA_PCM), ENTELEA_OK);

	/* 未授权采集拒（av:read 门）。 */
	entelea_perm_table_init(&t);
	entelea_principal_t poor;

	zassert_equal(entelea_principal_bind(&poor, 2, &t), ENTELEA_OK);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, poor),
		      ENTELEA_E_PERM);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_IDLE, "拒绝路径零启动副作用");
	zassert_equal(entelea_media_fake_start_calls(ENTELEA_MEDIA_PCM), 0,
		      "拒绝路径 start_calls==0（P3 修复：计数面断言）");

	/* 授权启动+取帧。 */
	zassert_equal(entelea_perm_parse("av:read:0", &t), ENTELEA_OK);
	entelea_principal_t ok_ctx;

	zassert_equal(entelea_principal_bind(&ok_ctx, 1, &t), ENTELEA_OK);
	static const uint8_t pat[4] = {0xDE, 0xAD, 0xBE, 0xEF};

	entelea_media_fake_set_frames(ENTELEA_MEDIA_PCM, pat, 4, 2);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, ok_ctx),
		      ENTELEA_OK);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_CAPTURING);
	zassert_equal(entelea_media_fake_start_calls(ENTELEA_MEDIA_PCM), 1,
		      "正向控制");
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f), ENTELEA_OK);
	zassert_equal(f.len, 4);
	zassert_ok(memcmp(f.bytes, pat, 4));
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f), ENTELEA_OK);
	/* 帧耗尽=E_BUSY（非失败）。 */
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f), ENTELEA_E_BUSY,
		      "无帧非失败");

	/* 停止幂等；停止后 poll 不复活。 */
	entelea_media_capture_stop(ENTELEA_MEDIA_PCM);
	entelea_media_capture_stop(ENTELEA_MEDIA_PCM);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_IDLE);
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f),
		      ENTELEA_E_STATE, "停止不复活");

	/* 重复启动拒。 */
	entelea_media_fake_set_frames(ENTELEA_MEDIA_PCM, pat, 4, 1);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, ok_ctx),
		      ENTELEA_OK);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, ok_ctx),
		      ENTELEA_E_BUSY, "重复启动显式拒");
	entelea_media_capture_stop(ENTELEA_MEDIA_PCM);
}

ZTEST(framework_media, test_backend_failure_explicit)
{
	entelea_media_test_reset();
	entelea_perm_test_reset();
	static const uint8_t pat[2] = {1, 2};

	zassert_equal(bind_fake(ENTELEA_MEDIA_PCM), ENTELEA_OK);
	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("av:read:0", &t), ENTELEA_OK);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 3, &t), ENTELEA_OK);

	/* start 失败显式（不静默成功/不置 CAPTURING）。 */
	entelea_media_fake_fail_next(ENTELEA_MEDIA_PCM);
	entelea_media_fake_set_frames(ENTELEA_MEDIA_PCM, pat, 2, 1);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, ctx),
		      ENTELEA_E_IO);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_IDLE);

	/* poll 失败 → FAILED 态（后端失败不报告成功）。 */
	entelea_media_fake_set_frames(ENTELEA_MEDIA_PCM, pat, 2, 2);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, ctx),
		      ENTELEA_OK);
	entelea_media_frame_t f;

	entelea_media_fake_fail_next(ENTELEA_MEDIA_PCM);
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f), ENTELEA_E_IO);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_FAILED, "失败态可观测");
	/* FAILED 后 poll=E_STATE（不复活——须重启采集）。 */
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f),
		      ENTELEA_E_STATE);
	/* 重启恢复。 */
	entelea_media_fake_set_frames(ENTELEA_MEDIA_PCM, pat, 2, 1);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, ctx),
		      ENTELEA_OK);
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f), ENTELEA_OK);
	entelea_media_capture_stop(ENTELEA_MEDIA_PCM);

	/* 绑定面负测：缺 ops 拒；类外拒。 */
	entelea_media_backend_t broken = entelea_media_fake_backend;

	broken.poll = NULL;
	zassert_equal(entelea_media_bind(ENTELEA_MEDIA_PCM, broken, NULL),
		      ENTELEA_E_PARAM);
	zassert_not_equal(
		entelea_media_capture_start(ENTELEA_MEDIA_PCM, ctx),
		ENTELEA_E_UNINIT, "坏绑定不改既有绑定");
	zassert_equal(entelea_media_bind(ENTELEA_MEDIA_CLASS_COUNT,
					 entelea_media_fake_backend, NULL),
		      ENTELEA_E_PARAM, "类外拒");
	entelea_media_capture_stop(ENTELEA_MEDIA_PCM);
}

/* ---- 按类独立（计划符合性审查 P2）：独立槽/独立生命周期/重绑不串权 ---- */
ZTEST(framework_media, test_class_independence)
{
	entelea_media_test_reset();
	entelea_perm_test_reset();
	static const uint8_t img[2] = {0xAA, 0x01};
	static const uint8_t pcm[2] = {0xBB, 0x02};

	zassert_equal(bind_fake(ENTELEA_MEDIA_IMAGE), ENTELEA_OK);
	zassert_equal(bind_fake(ENTELEA_MEDIA_PCM), ENTELEA_OK);

	entelea_perm_table_t t;

	entelea_perm_table_init(&t);
	zassert_equal(entelea_perm_parse("av:read:0", &t), ENTELEA_OK);
	entelea_principal_t ctx;

	zassert_equal(entelea_principal_bind(&ctx, 7, &t), ENTELEA_OK);

	/* 只启 IMAGE：PCM 保持 IDLE、零 start 调用。 */
	entelea_media_fake_set_frames(ENTELEA_MEDIA_IMAGE, img, 2, 1);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_IMAGE, ctx),
		      ENTELEA_OK);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_IMAGE),
		      ENTELEA_MEDIA_CAPTURING);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_IDLE, "他类不受影响");
	zassert_equal(entelea_media_fake_start_calls(ENTELEA_MEDIA_PCM), 0,
		      "按类计数独立");

	/* 停 IMAGE 不停 PCM 面；PCM 独立启动/独立帧内容。 */
	entelea_media_capture_stop(ENTELEA_MEDIA_IMAGE);
	entelea_media_fake_set_frames(ENTELEA_MEDIA_PCM, pcm, 2, 1);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_PCM, ctx),
		      ENTELEA_OK);
	entelea_media_frame_t f;

	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_PCM, &f), ENTELEA_OK);
	zassert_ok(memcmp(f.bytes, pcm, 2), "帧内容按类独立（不串槽）");
	zassert_equal(entelea_media_fake_start_calls(ENTELEA_MEDIA_IMAGE), 1,
		      "IMAGE 计数未被 PCM 启动改变");

	/* 重绑 IMAGE（替换）不影响进行中的 PCM。 */
	static const uint8_t img2[2] = {0xCC, 0x03};

	zassert_equal(bind_fake(ENTELEA_MEDIA_IMAGE), ENTELEA_OK);
	zassert_equal(entelea_media_state(ENTELEA_MEDIA_PCM),
		      ENTELEA_MEDIA_CAPTURING, "重绑他类不影响 PCM");
	entelea_media_fake_set_frames(ENTELEA_MEDIA_IMAGE, img2, 2, 1);
	zassert_equal(entelea_media_capture_start(ENTELEA_MEDIA_IMAGE, ctx),
		      ENTELEA_OK);
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_IMAGE, &f), ENTELEA_OK);
	zassert_ok(memcmp(f.bytes, img2, 2), "重绑后新内容生效");

	/* 类外参数拒。 */
	zassert_equal(entelea_media_poll(ENTELEA_MEDIA_CLASS_COUNT, &f),
		      ENTELEA_E_PARAM);
	entelea_media_capture_stop(ENTELEA_MEDIA_PCM);
	entelea_media_capture_stop(ENTELEA_MEDIA_IMAGE);
}

ZTEST_SUITE(framework_media, NULL, media_setup, NULL, NULL, NULL);
