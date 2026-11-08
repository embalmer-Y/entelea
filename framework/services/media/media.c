/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R09 可选媒体接口（新-only；合同见本目录 contract.md）。
 * 独立可选：每类媒体（图像/PCM/显示）独立绑定后端槽——重绑同类=替换、
 * 不影响他类（计划 L165：不得经单全局绑定串权；计划符合性审查 P2 修复：
 * 原实现是单 static 后端，二次 bind 覆盖他类）。
 * 采集≠保存/外发/记忆（权限分离）。假后端（ENTELEA_TEST，按类独立
 * 状态）验证接口/生命周期/取消/失败语义；真后端（S3 es8311/PDM/
 * P4 显示）随板级批接入同一 ops 形状。
 */
#include <string.h>
#include <entelea/media.h>
#include <entelea/principal.h>

static entelea_media_backend_t backends[ENTELEA_MEDIA_CLASS_COUNT];
static void *backend_users[ENTELEA_MEDIA_CLASS_COUNT];
static bool bounds[ENTELEA_MEDIA_CLASS_COUNT];

static bool class_ok(entelea_media_class_t cls)
{
	return (int)cls >= 0 && cls < ENTELEA_MEDIA_CLASS_COUNT;
}

entelea_result_t entelea_media_bind(entelea_media_class_t cls,
				    entelea_media_backend_t b, void *user)
{
	if (!class_ok(cls)) {
		return ENTELEA_E_PARAM;
	}
	if (b.start == NULL || b.poll == NULL || b.stop == NULL ||
	    b.state == NULL) {
		return ENTELEA_E_PARAM;
	}
	backends[cls] = b;
	backend_users[cls] = user;
	bounds[cls] = true;
	return ENTELEA_OK;
}

entelea_result_t entelea_media_capture_start(entelea_media_class_t cls,
					     entelea_principal_t ctx)
{
	if (!class_ok(cls)) {
		return ENTELEA_E_PARAM;
	}
	if (!bounds[cls]) {
		return ENTELEA_E_UNINIT;
	}
	/* 权限门：av:read（采集——不是保存/外发权限）。 */
	if (entelea_perm_check(ctx, ENTELEA_PERM_AV, ENTELEA_PERM_READ, 0) !=
	    ENTELEA_OK) {
		return ENTELEA_E_PERM;
	}
	if (backends[cls].state(backend_users[cls]) ==
	    ENTELEA_MEDIA_CAPTURING) {
		return ENTELEA_E_BUSY; /* 重复启动拒（显式） */
	}
	return backends[cls].start(backend_users[cls]);
}

entelea_result_t entelea_media_poll(entelea_media_class_t cls,
				    entelea_media_frame_t *out)
{
	if (!class_ok(cls)) {
		return ENTELEA_E_PARAM;
	}
	if (!bounds[cls]) {
		return ENTELEA_E_UNINIT;
	}
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (backends[cls].state(backend_users[cls]) != ENTELEA_MEDIA_CAPTURING) {
		return ENTELEA_E_STATE; /* 未采集/已停——不复活 */
	}
	return backends[cls].poll(backend_users[cls], out);
}

void entelea_media_capture_stop(entelea_media_class_t cls)
{
	if (class_ok(cls) && bounds[cls]) {
		backends[cls].stop(backend_users[cls]); /* 后端幂等 */
	}
}

entelea_media_state_t entelea_media_state(entelea_media_class_t cls)
{
	if (!class_ok(cls) || !bounds[cls]) {
		return ENTELEA_MEDIA_IDLE; /* 未绑定=IDLE（不伪装可用） */
	}
	return backends[cls].state(backend_users[cls]);
}

/* ---- 假后端（ENTELEA_TEST；按类独立状态——独立性判别用） ---- */
#ifdef ENTELEA_TEST

static struct {
	entelea_media_state_t state;
	uint8_t data[ENTELEA_MEDIA_FRAME_MAX];
	uint32_t len;
	uint32_t remaining;
	bool fail_next;
	uint32_t start_calls;
	uint64_t now;
} fake[ENTELEA_MEDIA_CLASS_COUNT];

void entelea_media_fake_set_frames(entelea_media_class_t cls,
				   const uint8_t *data, uint32_t len,
				   uint32_t count)
{
	if (!class_ok(cls)) {
		return;
	}
	if (len > ENTELEA_MEDIA_FRAME_MAX) {
		len = ENTELEA_MEDIA_FRAME_MAX;
	}
	memcpy(fake[cls].data, data, len);
	fake[cls].len = len;
	fake[cls].remaining = count;
}

void entelea_media_fake_fail_next(entelea_media_class_t cls)
{
	if (class_ok(cls)) {
		fake[cls].fail_next = true;
	}
}

uint32_t entelea_media_fake_start_calls(entelea_media_class_t cls)
{
	return class_ok(cls) ? fake[cls].start_calls : 0;
}

static entelea_result_t fake_start(void *user)
{
	entelea_media_class_t cls = (entelea_media_class_t)(uintptr_t)user;

	fake[cls].start_calls++;
	if (fake[cls].fail_next) {
		fake[cls].fail_next = false;
		return ENTELEA_E_IO;
	}
	fake[cls].state = ENTELEA_MEDIA_CAPTURING;
	return ENTELEA_OK;
}

static entelea_result_t fake_poll(void *user, entelea_media_frame_t *out)
{
	entelea_media_class_t cls = (entelea_media_class_t)(uintptr_t)user;

	if (fake[cls].fail_next) {
		fake[cls].fail_next = false;
		fake[cls].state = ENTELEA_MEDIA_FAILED;
		return ENTELEA_E_IO; /* 失败显式（不静默成功） */
	}
	if (fake[cls].remaining == 0) {
		return ENTELEA_E_BUSY; /* 无帧=非失败 */
	}
	fake[cls].remaining--;
	fake[cls].now++;
	out->t_ms = fake[cls].now;
	out->seq = fake[cls].start_calls * 1000 + (5 - fake[cls].remaining);
	out->len = fake[cls].len;
	memcpy(out->bytes, fake[cls].data, fake[cls].len);
	return ENTELEA_OK;
}

static void fake_stop(void *user)
{
	entelea_media_class_t cls = (entelea_media_class_t)(uintptr_t)user;

	fake[cls].state = ENTELEA_MEDIA_IDLE;
}

static entelea_media_state_t fake_state(void *user)
{
	entelea_media_class_t cls = (entelea_media_class_t)(uintptr_t)user;

	return fake[cls].state;
}

entelea_media_backend_t entelea_media_fake_backend = {
	.start = fake_start,
	.poll = fake_poll,
	.stop = fake_stop,
	.state = fake_state,
};

void entelea_media_test_reset(void)
{
	memset(fake, 0, sizeof(fake));
	memset(backends, 0, sizeof(backends));
	memset(backend_users, 0, sizeof(backend_users));
	memset(bounds, 0, sizeof(bounds));
}
#endif
