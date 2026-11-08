/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R09 可选媒体接口（新-only；合同见本目录 contract.md）。
 * 独立可选：每类媒体独立绑定后端；采集≠保存/外发/记忆（权限分离）。
 * 假后端（ENTELEA_TEST）验证接口/生命周期/取消/失败语义；真后端
 * （S3 es8311/PDM/P4 显示）随板级批接入同一 ops 形状。
 */
#include <string.h>
#include <entelea/media.h>
#include <entelea/principal.h>

static entelea_media_backend_t backend;
static void *backend_user;
static bool bound;

entelea_result_t entelea_media_bind(entelea_media_backend_t b, void *user)
{
	if (b.start == NULL || b.poll == NULL || b.stop == NULL ||
	    b.state == NULL) {
		return ENTELEA_E_PARAM;
	}
	backend = b;
	backend_user = user;
	bound = true;
	return ENTELEA_OK;
}

entelea_result_t entelea_media_capture_start(entelea_principal_t ctx)
{
	if (!bound) {
		return ENTELEA_E_UNINIT;
	}
	/* 权限门：av:read（采集——不是保存/外发权限）。 */
	if (entelea_perm_check(ctx, ENTELEA_PERM_AV, ENTELEA_PERM_READ, 0) !=
	    ENTELEA_OK) {
		return ENTELEA_E_PERM;
	}
	if (backend.state(backend_user) == ENTELEA_MEDIA_CAPTURING) {
		return ENTELEA_E_BUSY; /* 重复启动拒（显式） */
	}
	return backend.start(backend_user);
}

entelea_result_t entelea_media_poll(entelea_media_frame_t *out)
{
	if (!bound) {
		return ENTELEA_E_UNINIT;
	}
	if (out == NULL) {
		return ENTELEA_E_PARAM;
	}
	if (backend.state(backend_user) != ENTELEA_MEDIA_CAPTURING) {
		return ENTELEA_E_STATE; /* 未采集/已停——不复活 */
	}
	return backend.poll(backend_user, out);
}

void entelea_media_capture_stop(void)
{
	if (bound) {
		backend.stop(backend_user); /* 后端幂等 */
	}
}

entelea_media_state_t entelea_media_state(void)
{
	return bound ? backend.state(backend_user) : ENTELEA_MEDIA_IDLE;
}

/* ---- 假后端（ENTELEA_TEST） ---- */
#ifdef ENTELEA_TEST

static struct {
	entelea_media_state_t state;
	uint8_t data[ENTELEA_MEDIA_FRAME_MAX];
	uint32_t len;
	uint32_t remaining;
	bool fail_next;
	uint32_t start_calls;
	uint64_t now;
} fake;

void entelea_media_fake_set_frames(const uint8_t *data, uint32_t len,
				    uint32_t count)
{
	if (len > ENTELEA_MEDIA_FRAME_MAX) {
		len = ENTELEA_MEDIA_FRAME_MAX;
	}
	memcpy(fake.data, data, len);
	fake.len = len;
	fake.remaining = count;
}

void entelea_media_fake_fail_next(void)
{
	fake.fail_next = true;
}

uint32_t entelea_media_fake_start_calls(void)
{
	return fake.start_calls;
}

static entelea_result_t fake_start(void *user)
{
	(void)user;
	fake.start_calls++;
	if (fake.fail_next) {
		fake.fail_next = false;
		return ENTELEA_E_IO;
	}
	fake.state = ENTELEA_MEDIA_CAPTURING;
	return ENTELEA_OK;
}

static entelea_result_t fake_poll(void *user, entelea_media_frame_t *out)
{
	(void)user;
	if (fake.fail_next) {
		fake.fail_next = false;
		fake.state = ENTELEA_MEDIA_FAILED;
		return ENTELEA_E_IO; /* 失败显式（不静默成功） */
	}
	if (fake.remaining == 0) {
		return ENTELEA_E_BUSY; /* 无帧=非失败 */
	}
	fake.remaining--;
	fake.now++;
	out->t_ms = fake.now;
	out->seq = fake.start_calls * 1000 + (5 - fake.remaining);
	out->len = fake.len;
	memcpy(out->bytes, fake.data, fake.len);
	return ENTELEA_OK;
}

static void fake_stop(void *user)
{
	(void)user;
	fake.state = ENTELEA_MEDIA_IDLE;
}

static entelea_media_state_t fake_state(void *user)
{
	(void)user;
	return fake.state;
}

entelea_media_backend_t entelea_media_fake_backend = {
	.start = fake_start,
	.poll = fake_poll,
	.stop = fake_stop,
	.state = fake_state,
};

void entelea_media_test_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&backend, 0, sizeof(backend));
	bound = false;
}
#endif
