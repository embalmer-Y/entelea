/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_MEDIA_H
#define ENTELEA_MEDIA_H

/* R09 可选媒体接口合同见 framework/services/media/contract.md。
 * 图像/PCM/显示独立可选；采集≠保存/外发/记忆。假后端验证接口与
 * 生命周期/权限/取消语义；真后端随板级批。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_MEDIA_FRAME_MAX 512 /* 机制批帧上限（建议值） */

typedef enum {
	ENTELEA_MEDIA_IDLE = 0,
	ENTELEA_MEDIA_CAPTURING,
	ENTELEA_MEDIA_FAILED,
} entelea_media_state_t;

typedef struct {
	uint64_t t_ms;
	uint32_t seq;
	uint8_t bytes[ENTELEA_MEDIA_FRAME_MAX];
	uint32_t len;
} entelea_media_frame_t;

/* 媒体类（每类独立绑定/独立实例——计划 L165：不得经单全局绑定串权）。 */
typedef enum {
	ENTELEA_MEDIA_IMAGE = 0, /* 图像（摄像头帧） */
	ENTELEA_MEDIA_PCM,       /* 音频（PCM 采样） */
	ENTELEA_MEDIA_DISPLAY,   /* 显示（播放面归后续批——绑定/生命周期
				  * 机制面与采集类同形） */
	ENTELEA_MEDIA_CLASS_COUNT,
} entelea_media_class_t;

/* 后端 ops（假后端测试注入；真后端=板级实现同一形状）。 */
typedef struct {
	/* 启动采集（失败=E_IO）；frame_ready 轮询面。 */
	entelea_result_t (*start)(void *user);
	/* 取一帧（无=E_BUSY 有帧待读；失败=FAILED 态）。 */
	entelea_result_t (*poll)(void *user, entelea_media_frame_t *out);
	/* 停止（幂等）；取消中进行中读不复活）。 */
	void (*stop)(void *user);
	entelea_media_state_t (*state)(void *user);
} entelea_media_backend_t;

/** [thread] 绑定后端（各媒体类独立槽：重绑同类=替换，不影响他类；
 * 类外=E_PARAM）。 */
entelea_result_t entelea_media_bind(entelea_media_class_t cls,
				    entelea_media_backend_t backend, void *user);

/** [thread] 采集启动：权限门（av:read 声明）→ 后端 start。无授权
 * 保存/外发（本面仅采集——保存/外发是独立权限与独立面）。 */
entelea_result_t entelea_media_capture_start(entelea_media_class_t cls,
					     entelea_principal_t ctx);

/** [thread] 取帧（无帧=E_BUSY——非失败；后端失败置 FAILED 态）。 */
entelea_result_t entelea_media_poll(entelea_media_class_t cls,
				    entelea_media_frame_t *out);

/** [thread] 停止（幂等）。停止后 poll=E_STATE（不复活）。 */
void entelea_media_capture_stop(entelea_media_class_t cls);

/** [any] 状态（观测；未绑定类=IDLE 不伪装可用）。 */
entelea_media_state_t entelea_media_state(entelea_media_class_t cls);

#ifdef ENTELEA_TEST
void entelea_media_test_reset(void);
/* 假后端注入（测试）。 */
extern entelea_media_backend_t entelea_media_fake_backend;
/* 假后端控制面（按类独立状态——独立性判别用）。 */
void entelea_media_fake_set_frames(entelea_media_class_t cls,
				   const uint8_t *data, uint32_t len,
				   uint32_t count);
void entelea_media_fake_fail_next(entelea_media_class_t cls);
uint32_t entelea_media_fake_start_calls(entelea_media_class_t cls);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_MEDIA_H */
