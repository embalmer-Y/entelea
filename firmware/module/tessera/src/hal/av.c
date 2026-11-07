// SPDX-License-Identifier: Apache-2.0
/* ts-av 能力面（DEC-47③⑥，MD1.2g）：最小采集面 ts_av_capture（阻塞取一帧
 * 到 APP 缓冲）+ 发布权限面 ts_av_publish（经 ts-net avq 分片通道——
 * 1KB chunk/打拍≥4ms/重试≤5×20ms = DEC-47②，net 线程单发送）。
 * 格式/分辨率经 manifest（av_fmt/av_w/av_h）声明 → ts_av_config_bind 随载
 * 绑定；未绑定/未授权 = fail-closed。采集/发布均为读类（合同 3
 * ——不经安全提交层；权限裁决走 ts_perm_check，合同 10）。
 * 摄像头 = chosen zephyr,camera（官方 video_buffer_aligned_alloc 池路径，
 * MD1.2b 结论）；无 chosen 板（native_sim/测试）→ TS_E_IO 如实失败。
 */
#include <errno.h>
#include <string.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <zephyr/kernel.h>

#define PCM16_FRAME_BYTES 640U

#if defined(CONFIG_TS_NET)
#include <ts/net.h>
#endif

#if DT_HAS_CHOSEN(zephyr_camera)
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#define AV_CAM DEVICE_DT_GET(DT_CHOSEN(zephyr_camera))

static ts_res_t av_stream_release(void);
#endif

static K_MUTEX_DEFINE(av_lock);

/* 配置绑定（V1 单活跃 APP = 单条目；覆盖式——与 fs_paths 同范式） */
static struct {
	uint16_t app;
	ts_av_fmt_t fmt;
	uint16_t w, h;
	bool valid;
} av_cfg = {
	.app = 0xFFFF,
};

static ts_res_t av_config_bind(uint16_t app_id, ts_av_fmt_t fmt, uint16_t w, uint16_t h)
{
	if (fmt == TS_AV_FMT_PCM16) {
		/* Q-36/DEC-54②：mono 16kHz 16bit（w = 16 kHz，h 弃用）。
		 */
		if (w != 16) {
			return TS_E_PARAM;
		}
		h = 0;
	} else {
		if ((fmt != TS_AV_FMT_JPEG && fmt != TS_AV_FMT_RGB565) || w < 16 || w > 800 ||
		    h < 16 || h > 800) {
			return TS_E_PARAM;
		}
	}
#if DT_HAS_CHOSEN(zephyr_camera)
	if ((!av_cfg.valid || av_cfg.app != app_id || av_cfg.fmt != fmt || av_cfg.w != w ||
	     av_cfg.h != h) &&
	    av_stream_release() != TS_OK) {
		return TS_E_IO;
	}
#endif
	av_cfg.app = app_id;
	av_cfg.fmt = fmt;
	av_cfg.w = w;
	av_cfg.h = h;
	av_cfg.valid = true;
	return TS_OK;
}

ts_res_t ts_av_config_bind_ctx(ts_ctx_t c, ts_av_fmt_t fmt, uint16_t w, uint16_t h)
{
	return ts_av_config_bind(c.app_id, fmt, w, h);
}

#if DT_HAS_CHOSEN(zephyr_camera)
static bool stream_started;
static bool restart_pending;
static bool restart_stopped;
static bool release_pending;
static bool stream_needs_stop;
static struct video_buffer *pending_requeue;
static struct {
	struct video_buffer *buffer;
	bool queued;
} stream_buffers[2];
/* V2-1 自愈（#121774 workaround）：连续 dequeue 超时计数——上游缺陷形态
 * = buffer 耗尽后 DMA 永久 parked（每帧必超时）；达 RECOVER_N 即重启流。
 */
static uint8_t dequeue_timeouts;

static ts_res_t av_buffer_returned(struct video_buffer *buffer)
{
	for (size_t i = 0; i < ARRAY_SIZE(stream_buffers); i++) {
		if (buffer && stream_buffers[i].buffer == buffer && stream_buffers[i].queued) {
			stream_buffers[i].queued = false;
			return TS_OK;
		}
	}
	printk("[av] unexpected dequeued buffer\n");
	return TS_E_IO;
}

static ts_res_t av_stream_release(void)
{
	int ret;

	release_pending = true;
	if (stream_needs_stop && !restart_stopped) {
		ret = video_stream_stop(AV_CAM, VIDEO_BUF_TYPE_OUTPUT);
		if (ret != 0) {
			printk("[av] cleanup stop failed rc=%d\n", ret);
			return TS_E_IO;
		}
		restart_stopped = true;
	}
	for (size_t i = 0; i < ARRAY_SIZE(stream_buffers); i++) {
		if (!stream_buffers[i].queued) {
			continue;
		}
		/* video_stream_stop discards the flush result in pinned Zephyr. */
		ret = video_driver_flush(AV_CAM, true);
		if (ret != 0) {
			printk("[av] cleanup flush failed rc=%d\n", ret);
			return TS_E_IO;
		}
		break;
	}
	for (size_t i = 0; i < ARRAY_SIZE(stream_buffers); i++) {
		struct video_buffer *buffer;
		bool queued = false;

		for (size_t j = 0; j < ARRAY_SIZE(stream_buffers); j++) {
			queued |= stream_buffers[j].queued;
		}
		if (!queued) {
			break;
		}
		ret = video_dequeue(AV_CAM, &buffer, K_NO_WAIT);
		if (ret != 0) {
			printk("[av] cleanup dequeue failed rc=%d\n", ret);
			return TS_E_IO;
		}
		if (av_buffer_returned(buffer) != TS_OK) {
			return TS_E_IO;
		}
	}
	pending_requeue = NULL;
	for (size_t i = 0; i < ARRAY_SIZE(stream_buffers); i++) {
		if (!stream_buffers[i].buffer) {
			continue;
		}
		ret = video_buffer_release(stream_buffers[i].buffer);
		if (ret != 0) {
			printk("[av] buffer release failed rc=%d\n", ret);
			return TS_E_IO;
		}
		stream_buffers[i].buffer = NULL;
	}
	stream_started = false;
	stream_needs_stop = false;
	restart_pending = false;
	restart_stopped = false;
	dequeue_timeouts = 0;
	release_pending = false;
	return TS_OK;
}

/* 流重启（维护者确认的恢复路径）：stop（active_vbuf 排入 fifo_out）→
 * 取回完成帧重新入队（全部 buffer 归 fifo_in）→ start（驱动重新 arm DMA）。
 * parked 形态下 active_vbuf==NULL，fifo_out 为空、fifo_in 完整——直接
 * start 即恢复。
 */
static ts_res_t av_requeue_pending(void)
{
	int ret;

	if (!pending_requeue) {
		return TS_OK;
	}
	ret = video_enqueue(AV_CAM, pending_requeue);
	if (ret != 0) {
		printk("[av] requeue failed rc=%d\n", ret);
		return TS_E_IO;
	}
	for (size_t i = 0; i < ARRAY_SIZE(stream_buffers); i++) {
		if (stream_buffers[i].buffer == pending_requeue) {
			stream_buffers[i].queued = true;
			break;
		}
	}
	pending_requeue = NULL;
	return TS_OK;
}

static ts_res_t av_stream_restart(void)
{
	struct video_buffer *vb;
	int ret;

	if (!restart_stopped) {
		restart_pending = true;
		printk("[av] stream restart (self-heal)\n");
		ret = video_stream_stop(AV_CAM, VIDEO_BUF_TYPE_OUTPUT);
		if (ret != 0) {
			printk("[av] stream stop failed rc=%d\n", ret);
			return TS_E_IO;
		}
		restart_stopped = true;
	}
	if (av_requeue_pending() != TS_OK) {
		return TS_E_IO;
	}
	while ((ret = video_dequeue(AV_CAM, &vb, K_NO_WAIT)) == 0) {
		if (av_buffer_returned(vb) != TS_OK) {
			return TS_E_IO;
		}
		pending_requeue = vb;
		if (av_requeue_pending() != TS_OK) {
			return TS_E_IO;
		}
	}
	if (ret != -EAGAIN) {
		printk("[av] restart dequeue failed rc=%d\n", ret);
		return TS_E_IO;
	}
	restart_stopped = false;
	ret = video_stream_start(AV_CAM, VIDEO_BUF_TYPE_OUTPUT);
	if (ret != 0) {
		printk("[av] stream start failed rc=%d\n", ret);
		return TS_E_IO;
	}
	restart_pending = false;
	restart_stopped = false;
	return TS_OK;
}

/* 官方池路径（MD1.2b）：缓冲 w*h*2（RGB565 定长；JPEG 上界同宽——驱动按
 * bytesused 上报实际压缩长）。池堆 = 板级 VIDEO_BUFFER_POOL 配置。
 */
static int ensure_stream(void)
{
	ts_res_t result;

	if (release_pending && av_stream_release() != TS_OK) {
		return TS_E_IO;
	}
	if (stream_started) {
		return TS_OK;
	}
	if (!device_is_ready(AV_CAM)) {
		printk("[av] camera not ready\n");
		return TS_E_IO;
	}
	struct video_format fmt = {
		.pixelformat =
			(av_cfg.fmt == TS_AV_FMT_JPEG) ? VIDEO_PIX_FMT_JPEG : VIDEO_PIX_FMT_RGB565,
		.width = av_cfg.w,
		.height = av_cfg.h,
		.pitch = (av_cfg.fmt == TS_AV_FMT_JPEG) ? 0 : av_cfg.w * 2,
	};

	if (video_set_format(AV_CAM, &fmt) != 0) {
		printk("[av] set_format fail (%ux%u fmt=%d)\n", av_cfg.w, av_cfg.h,
		       (int)fmt.pixelformat);
		return TS_E_IO;
	}
	const uint32_t frame_bytes = (uint32_t)av_cfg.w * av_cfg.h * 2;

	for (size_t i = 0; i < ARRAY_SIZE(stream_buffers); i++) {
		struct video_buffer *vb = video_buffer_aligned_alloc(frame_bytes, 32, K_NO_WAIT);

		if (!vb) {
			printk("[av] buf alloc fail\n");
			result = TS_E_NOMEM;
			goto fail;
		}
		vb->type = VIDEO_BUF_TYPE_OUTPUT;
		stream_buffers[i].buffer = vb;
	}
	for (size_t i = 0; i < ARRAY_SIZE(stream_buffers); i++) {
		if (video_enqueue(AV_CAM, stream_buffers[i].buffer) != 0) {
			printk("[av] enqueue fail\n");
			result = TS_E_IO;
			goto fail;
		}
		stream_buffers[i].queued = true;
	}
	/* A failed start may already have armed DMA. Stop before releasing memory. */
	stream_needs_stop = true;
	if (video_stream_start(AV_CAM, VIDEO_BUF_TYPE_OUTPUT) != 0) {
		printk("[av] stream start fail\n");
		result = TS_E_IO;
		goto fail;
	}
	printk("[av] stream started (%ux%u fmt=%s)\n", av_cfg.w, av_cfg.h,
	       av_cfg.fmt == TS_AV_FMT_JPEG ? "jpeg" : "rgb565");
	stream_started = true;
	return TS_OK;

fail:
	return av_stream_release() == TS_OK ? result : TS_E_IO;
}
#endif /* DT_HAS_CHOSEN(zephyr_camera) */

static ts_res_t av_capture(ts_ctx_t c, uint8_t *buf, uint32_t cap, uint32_t *len)
{
	if (!buf || !len || cap == 0) {
		return TS_E_PARAM;
	}
	ts_res_t pr = ts_perm_check(c, TS_PERM_CLASS_AV, TS_PERM_OP_READ, 0);

	if (pr != TS_OK) {
		return pr;
	}
	if (!av_cfg.valid || c.app_id != av_cfg.app) {
		return TS_E_STATE; /* 未声明 av_fmt/av_w/av_h = fail-closed */
	}
	if (av_cfg.fmt == TS_AV_FMT_PCM16) {
		/* Q-36/DEC-54②：音频帧 = 640B 定长（320 采样 × 2B）。 */
		if (cap < PCM16_FRAME_BYTES) {
			return TS_E_RANGE;
		}
#if defined(CONFIG_TS_DRV_PDM)
		/* 惰性初始化：缺少初始化时后端不能采集。
		 */
		if (ts_drv_pdm_init() != 0) {
			return TS_E_IO;
		}
		int r = ts_drv_pdm_capture(buf, PCM16_FRAME_BYTES);

		if (r != PCM16_FRAME_BYTES) {
			printk("[av] pdm capture length=%d\n", r);
			return TS_E_IO;
		}
		*len = PCM16_FRAME_BYTES;
		return TS_OK;
#elif defined(CONFIG_TS_DRV_ES8311)
		/* K2（Q-44①）：ES8311 ADC 主源，全 codec 板使用。
		 */
		if (ts_drv_es8311_init() != 0) {
			return TS_E_IO;
		}
		{
			int r = ts_drv_es8311_capture(buf, PCM16_FRAME_BYTES);

			if (r != PCM16_FRAME_BYTES) {
				printk("[av] codec capture length=%d\n", r);
				return TS_E_IO;
			}
			*len = PCM16_FRAME_BYTES;
			return TS_OK;
		}
#elif defined(CONFIG_TS_TEST)
		/* TEST 确定性递增样本，真机使用硬件后端。 */
		static uint16_t pcm_seed;

		pcm_seed += 7;
		uint16_t *s = (uint16_t *)buf;

		for (uint32_t i = 0; i < PCM16_FRAME_BYTES / 2; i++) {
			s[i] = (uint16_t)(pcm_seed + i);
		}
		*len = PCM16_FRAME_BYTES;
		return TS_OK;
#else
		printk("[av] no pdm backend (stub path)\n");
		return TS_E_IO; /* 无 PDM 后端（板未配 TS_DRV_PDM）：如实失败 */
#endif
	}
#if DT_HAS_CHOSEN(zephyr_camera)
	if (!stream_started || release_pending) {
		ts_res_t sr = ensure_stream();

		if (sr != TS_OK) {
			return sr;
		}
	}
	if (restart_pending && av_stream_restart() != TS_OK) {
		return TS_E_IO;
	}
	if (av_requeue_pending() != TS_OK) {
		return TS_E_IO;
	}
	struct video_buffer *vb;

	/* 超时须 < TS_WDT_APPMGR_PERIOD_MS（DEC-48）：采集停顿先以 TS_E_IO
	 * 失败返回，不升级为看门狗复位。连续超时达 RECOVER_N（#121774
	 * parked 形态 = 每帧必超时）→ 流重启自愈；本轮仍如实返回 E_IO
	 * （重启后首帧需一个帧间隔，APP 重试语义下随即恢复）。
	 */
	if (video_dequeue(AV_CAM, &vb, K_MSEC(CONFIG_TS_HAL_AV_TIMEOUT_MS)) != 0) {
		if (CONFIG_TS_HAL_AV_RECOVER_N > 0 &&
		    ++dequeue_timeouts >= CONFIG_TS_HAL_AV_RECOVER_N) {
			dequeue_timeouts = 0;
			if (av_stream_restart() != TS_OK) {
				printk("[av] self-heal restart fail\n");
			}
		} else {
			printk("[av] dequeue timeout (%dms, n=%u)\n", CONFIG_TS_HAL_AV_TIMEOUT_MS,
			       dequeue_timeouts);
		}
		return TS_E_IO;
	}
	dequeue_timeouts = 0;
	if (av_buffer_returned(vb) != TS_OK) {
		return TS_E_IO;
	}
	ts_res_t r = TS_OK;

	if (vb->bytesused > vb->size) {
		printk("[av] invalid frame bytes=%u capacity=%u\n", vb->bytesused, vb->size);
		r = TS_E_IO;
	} else if (vb->bytesused > cap) {
		r = TS_E_RANGE; /* 帧大于 APP 缓冲（manifest 分辨率与缓冲不匹配） */
	} else {
		memcpy(buf, vb->buffer, vb->bytesused);
	}
	uint32_t frame_len = vb->bytesused;

	pending_requeue = vb;
	if (av_requeue_pending() != TS_OK) {
		return TS_E_IO;
	}
	if (r == TS_OK) {
		*len = frame_len;
	}
	return r;
#else
	printk("[av] no camera chosen (stub path)\n");
	return TS_E_IO; /* 无摄像头（native_sim/测试板）：如实失败 */
#endif
}

static ts_res_t av_write(ts_ctx_t c, const uint8_t *buf, uint32_t len)
{
	/* K2（Q-44②/DEC-61②）：音频输出面——权限 = av:write（位图加性，
	 * manifest av_fmt=pcm16 声明绑定同输入面）。
	 */
	if (!buf || len != PCM16_FRAME_BYTES) {
		return TS_E_PARAM; /* pcm16 mono 16k 定长帧（Q-36 语义对称） */
	}
	ts_res_t pr = ts_perm_check(c, TS_PERM_CLASS_AV, TS_PERM_OP_WRITE, 0);

	if (pr != TS_OK) {
		return pr;
	}
	if (!av_cfg.valid || c.app_id != av_cfg.app || av_cfg.fmt != TS_AV_FMT_PCM16) {
		return TS_E_STATE; /* 未声明 pcm16 = 输出面 fail-closed */
	}
#if defined(CONFIG_TS_DRV_ES8311)
	if (ts_drv_es8311_init() != 0) {
		return TS_E_IO;
	}
	int r = ts_drv_es8311_write(buf, len);

	if (r != PCM16_FRAME_BYTES) {
		printk("[av] codec write length=%d\n", r);
		return TS_E_IO;
	}
	return TS_OK;
#else
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	printk("[av] no output backend (stub path)\n");
	return TS_E_IO; /* 无输出后端（PDM = 纯输入；native_sim）：如实失败 */
#endif
}

ts_res_t ts_av_config_bind(uint16_t app_id, ts_av_fmt_t fmt, uint16_t w, uint16_t h)
{
	ts_res_t result;

	k_mutex_lock(&av_lock, K_FOREVER);
	result = av_config_bind(app_id, fmt, w, h);
	k_mutex_unlock(&av_lock);
	return result;
}

ts_res_t ts_av_capture(ts_ctx_t c, uint8_t *buf, uint32_t cap, uint32_t *len)
{
	ts_res_t result;

	k_mutex_lock(&av_lock, K_FOREVER);
	result = av_capture(c, buf, cap, len);
	k_mutex_unlock(&av_lock);
	return result;
}

ts_res_t ts_av_write(ts_ctx_t c, const uint8_t *buf, uint32_t len)
{
	ts_res_t result;

	k_mutex_lock(&av_lock, K_FOREVER);
	result = av_write(c, buf, len);
	k_mutex_unlock(&av_lock);
	return result;
}

ts_res_t ts_av_publish(ts_ctx_t c, uint32_t frame_id, uint32_t chunk_id, uint32_t n_chunks,
		       const uint8_t *data, uint32_t len)
{
#if defined(CONFIG_TS_NET)
	if (!data || len == 0 || len > CONFIG_TS_NET_PUBLISH_MAX_BYTES) {
		return TS_E_PARAM;
	}
	if (n_chunks == 0 || chunk_id >= n_chunks || n_chunks > 4096) {
		return TS_E_PARAM;
	}
	ts_res_t pr = ts_perm_check(c, TS_PERM_CLASS_AV, TS_PERM_OP_READ, 0);

	if (pr != TS_OK) {
		return pr;
	}
	/* 权限在 HAL 裁决；分片纪律在 ts-net avq。 */
	return ts_net_avq_push(c.app_id, frame_id, chunk_id, n_chunks, data, len);
#else
	ARG_UNUSED(c);
	ARG_UNUSED(frame_id);
	ARG_UNUSED(chunk_id);
	ARG_UNUSED(n_chunks);
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return TS_E_STATE; /* ts-net 未编入：发布面不可用（如实） */
#endif
}
