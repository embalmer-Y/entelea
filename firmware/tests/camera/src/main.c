// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <string.h>

#include <ts/hal.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/video/video.h>
#include <zephyr/ztest.h>

#define FRAME_BYTES (16U * 16U * 2U)
#define MAX_FRAME_BYTES (32U * 16U * 2U)
#define CAMERA_BUFFERS 2U

static struct video_buffer buffers[CAMERA_BUFFERS];
static uint8_t pixels[CAMERA_BUFFERS][MAX_FRAME_BYTES];
static struct video_buffer *queue[CAMERA_BUFFERS];
static struct video_buffer *completed[CAMERA_BUFFERS];
static unsigned int allocated;
static unsigned int queue_count;
static unsigned int completed_count;
static unsigned int dequeue_count;
static unsigned int enqueue_count;
static unsigned int start_count;
static unsigned int stop_count;
static unsigned int allocation_count;
static unsigned int release_count;
static unsigned int allocation_failure;
static unsigned int enqueue_failure_at;
static unsigned int flush_count;
static unsigned int format_count;
static unsigned int forced_timeouts;
static uint16_t expected_width;
static uint32_t expected_format;
static bool stopped;
static bool stop_failure;
static bool enqueue_failure;
static bool stopped_dequeue_failure;
static bool start_failure;
static bool format_failure;
static bool flush_failure;
static bool release_failure;
static bool allowed;
static bool block_capture;
static uint32_t reported_size;
static K_SEM_DEFINE(capture_entered, 0, 1);
static K_SEM_DEFINE(capture_resume, 0, 1);
static K_SEM_DEFINE(binding_entered, 0, 1);
static K_SEM_DEFINE(binding_finished, 0, 1);
static K_THREAD_STACK_DEFINE(capture_stack, 2048);
static K_THREAD_STACK_DEFINE(binding_stack, 2048);
static struct k_thread capture_thread;
static struct k_thread binding_thread;
static ts_res_t capture_result;
static ts_res_t binding_result;

static int camera_flush(const struct device *dev, bool cancel)
{
	ARG_UNUSED(dev);
	zassert_true(cancel);
	flush_count++;
	if (flush_failure) {
		return -EIO;
	}
	while (queue_count) {
		zassert_true(completed_count < CAMERA_BUFFERS);
		completed[completed_count++] = queue[--queue_count];
	}
	return 0;
}

static DEVICE_API(video, camera_api) = {
	.flush = camera_flush,
};

static int camera_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

DEVICE_DT_DEFINE(DT_CHOSEN(zephyr_camera), camera_init, NULL, NULL, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &camera_api);

ts_res_t ts_perm_check(ts_ctx_t ctx, ts_perm_class_t cls, ts_perm_op_t op, uint8_t inst)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(cls);
	ARG_UNUSED(op);
	ARG_UNUSED(inst);
	return allowed ? TS_OK : TS_E_PERM;
}

int video_set_format(const struct device *dev, struct video_format *format)
{
	zassert_true(device_is_ready(dev));
	format_count++;
	zassert_equal(format->width, expected_width);
	zassert_equal(format->height, 16);
	zassert_equal(format->pixelformat, expected_format);
	return format_failure ? -EIO : 0;
}

struct video_buffer *video_buffer_aligned_alloc(size_t size, size_t align, k_timeout_t timeout)
{
	struct video_buffer *buffer;

	ARG_UNUSED(timeout);
	zassert_equal(size, expected_width * 16U * 2U);
	zassert_equal(align, 32);
	allocation_count++;
	if (allocation_count == allocation_failure) {
		return NULL;
	}
	zassert_true(allocated < CAMERA_BUFFERS, "camera buffer leak");
	unsigned int index;

	for (index = 0; index < CAMERA_BUFFERS; index++) {
		if (!buffers[index].buffer) {
			break;
		}
	}
	zassert_true(index < CAMERA_BUFFERS);
	buffer = &buffers[index];
	buffer->buffer = pixels[index];
	buffer->size = size;
	memset(buffer->buffer, 0x5a + index, size);
	allocated++;
	return buffer;
}

int video_buffer_release(struct video_buffer *buffer)
{
	zassert_true(stopped, "released memory while DMA may still be running");
	zassert_not_null(buffer);
	zassert_not_null(buffer->buffer);
	for (unsigned int i = 0; i < queue_count; i++) {
		zassert_not_equal(queue[i], buffer, "released an enqueued buffer");
	}
	for (unsigned int i = 0; i < completed_count; i++) {
		zassert_not_equal(completed[i], buffer, "released an undrained buffer");
	}
	if (release_failure) {
		return -EIO;
	}
	buffer->buffer = NULL;
	allocated--;
	release_count++;
	return 0;
}

int video_enqueue(const struct device *dev, struct video_buffer *buffer)
{
	ARG_UNUSED(dev);
	enqueue_count++;
	if (enqueue_failure || enqueue_count == enqueue_failure_at) {
		return -EIO;
	}
	zassert_true(queue_count < CAMERA_BUFFERS);
	queue[queue_count++] = buffer;
	return 0;
}

int video_dequeue(const struct device *dev, struct video_buffer **buffer, k_timeout_t timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(timeout);
	dequeue_count++;
	if (!stopped && block_capture) {
		block_capture = false;
		k_sem_give(&capture_entered);
		zassert_equal(k_sem_take(&capture_resume, K_SECONDS(2)), 0);
	}
	if (!stopped && forced_timeouts) {
		forced_timeouts--;
		return -EAGAIN;
	}
	if (stopped) {
		if (stopped_dequeue_failure) {
			return -EIO;
		}
		if (!completed_count) {
			return -EAGAIN;
		}
		*buffer = completed[0];
		completed_count--;
		if (completed_count) {
			completed[0] = completed[1];
		}
		return 0;
	}
	if (!queue_count) {
		return -EAGAIN;
	}
	*buffer = queue[0];
	queue_count--;
	if (queue_count) {
		queue[0] = queue[1];
	}
	(*buffer)->bytesused = reported_size;
	return 0;
}

int video_stream_start(const struct device *dev, enum video_buf_type type)
{
	ARG_UNUSED(dev);
	zassert_equal(type, VIDEO_BUF_TYPE_OUTPUT);
	start_count++;
	stopped = false;
	if (start_failure) {
		return -EIO;
	}
	return 0;
}

int video_stream_stop(const struct device *dev, enum video_buf_type type)
{
	ARG_UNUSED(dev);
	zassert_equal(type, VIDEO_BUF_TYPE_OUTPUT);
	stop_count++;
	if (!stop_failure) {
		stopped = true;
	}
	/* Model the API wrapper's best-effort flush, including its ignored error. */
	camera_flush(dev, true);
	return stop_failure ? -EIO : 0;
}

static void reset_test(void *fixture)
{
	ARG_UNUSED(fixture);
	allocation_failure = 0;
	enqueue_failure_at = 0;
	forced_timeouts = 0;
	stop_failure = false;
	enqueue_failure = false;
	stopped_dequeue_failure = false;
	start_failure = false;
	format_failure = false;
	flush_failure = false;
	release_failure = false;
	allowed = true;
	block_capture = false;
	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_PCM16, 16, 0), TS_OK);
	zassert_equal(allocated, 0, "buffers survived configuration switch");
	zassert_equal(queue_count, 0);
	zassert_equal(completed_count, 0);
	memset(buffers, 0, sizeof(buffers));
	dequeue_count = 0;
	enqueue_count = 0;
	start_count = 0;
	stop_count = 0;
	allocation_count = 0;
	release_count = 0;
	flush_count = 0;
	format_count = 0;
	stopped = true;
	expected_width = 16;
	expected_format = VIDEO_PIX_FMT_RGB565;
	reported_size = FRAME_BYTES;
}

ZTEST(camera_contract, test_requeue_failure_and_retry)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES + 1];
	uint32_t len = 17;
	unsigned int previous_dequeues;

	reported_size = FRAME_BYTES;
	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(len, FRAME_BYTES);
	zassert_equal(frame[0], 0x5a);
	zassert_equal(allocated, CAMERA_BUFFERS);
	zassert_equal(start_count, 1);
	zassert_equal(queue_count, CAMERA_BUFFERS);

	enqueue_failure = true;
	len = 23;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 23);
	zassert_equal(queue_count, 1);
	previous_dequeues = dequeue_count;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 23);
	zassert_equal(dequeue_count, previous_dequeues);
	zassert_equal(queue_count, 1);

	enqueue_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(len, FRAME_BYTES);
	zassert_equal(queue_count, CAMERA_BUFFERS);
	zassert_equal(allocated, CAMERA_BUFFERS);
	zassert_equal(start_count, 1);
	zassert_equal(dequeue_count, previous_dequeues + 1);

	len = 31;
	zassert_equal(ts_av_capture(ctx, frame, FRAME_BYTES - 1, &len), TS_E_RANGE);
	zassert_equal(len, 31);
	zassert_equal(queue_count, CAMERA_BUFFERS);

	reported_size = FRAME_BYTES + 1;
	memset(frame, 0x33, sizeof(frame));
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 31);
	zassert_equal(frame[0], 0x33);
	zassert_equal(frame[FRAME_BYTES], 0x33);
	zassert_equal(queue_count, CAMERA_BUFFERS);

	reported_size = FRAME_BYTES;
	enqueue_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, FRAME_BYTES - 1, &len), TS_E_IO);
	zassert_equal(len, 31);
	zassert_equal(queue_count, 1);
	enqueue_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(queue_count, CAMERA_BUFFERS);

	forced_timeouts = 3;
	enqueue_failure = true;
	stop_failure = true;
	len = 47;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 47);
	zassert_false(stopped);
	zassert_equal(stop_count, 1);
	previous_dequeues = dequeue_count;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(dequeue_count, previous_dequeues);
	zassert_equal(stop_count, 2);

	stop_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_true(stopped);
	zassert_equal(stop_count, 3);
	zassert_equal(completed_count, 1);
	zassert_equal(queue_count, 0);
	previous_dequeues = dequeue_count;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(dequeue_count, previous_dequeues);
	zassert_equal(stop_count, 3);

	enqueue_failure = false;
	stopped_dequeue_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(queue_count, 1);
	zassert_equal(completed_count, 1);
	zassert_equal(stop_count, 3);

	stopped_dequeue_failure = false;
	start_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 47);
	zassert_equal(queue_count, CAMERA_BUFFERS);
	zassert_equal(completed_count, 0);
	zassert_equal(stop_count, 3);
	zassert_equal(start_count, 2);

	start_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(len, FRAME_BYTES);
	zassert_equal(queue_count, CAMERA_BUFFERS);
	zassert_equal(allocated, CAMERA_BUFFERS);
	zassert_equal(stop_count, 4);
	zassert_equal(start_count, 3);
	zassert_true(enqueue_count > 0);
}

ZTEST(camera_contract, test_initial_allocation_failure)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	allocation_failure = 1;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_NOMEM);
	zassert_equal(allocated, 0);
	zassert_equal(release_count, 0);
	allocation_failure = allocation_count + 2;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_NOMEM);
	zassert_equal(len, 17);
	zassert_equal(allocated, 0);
	zassert_equal(release_count, 1);
	zassert_equal(queue_count, 0);
	allocation_failure = 0;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(allocated, CAMERA_BUFFERS);
}

ZTEST(camera_contract, test_initial_enqueue_failure)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	enqueue_failure_at = 1;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(allocated, 0);
	zassert_equal(release_count, CAMERA_BUFFERS);
	enqueue_failure_at = enqueue_count + 2;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 17);
	zassert_equal(allocated, 0);
	zassert_equal(release_count, CAMERA_BUFFERS * 2);
	zassert_equal(queue_count, 0);
	enqueue_failure_at = 0;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
}

ZTEST(camera_contract, test_initial_start_failure)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	start_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 17);
	zassert_equal(stop_count, 1);
	zassert_equal(allocated, 0);
	zassert_equal(queue_count, 0);
	zassert_equal(completed_count, 0);
	start_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
}

ZTEST(camera_contract, test_cleanup_failure_blocks_reallocation)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	start_failure = true;
	stop_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(allocated, CAMERA_BUFFERS);
	start_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(allocation_count, CAMERA_BUFFERS);
	zassert_equal(release_count, 0);
	stop_failure = false;
	flush_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(len, 17);
	zassert_equal(allocation_count, CAMERA_BUFFERS);
	zassert_equal(release_count, 0);
	flush_failure = false;
	stopped_dequeue_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(release_count, 0);
	stopped_dequeue_failure = false;
	release_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(allocated, CAMERA_BUFFERS);
	release_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(release_count, CAMERA_BUFFERS);
	zassert_equal(allocation_count, CAMERA_BUFFERS * 2);
}

ZTEST(camera_contract, test_configuration_switch_and_owner)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[MAX_FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	zassert_equal(stop_count, 0);
	zassert_equal(ts_av_config_bind(43, TS_AV_FMT_RGB565, 32, 16), TS_OK);
	zassert_equal(stop_count, 1);
	zassert_equal(allocated, 0);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_STATE);
	expected_width = 32;
	reported_size = MAX_FRAME_BYTES;
	ctx.app_id = 43;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(len, MAX_FRAME_BYTES);
	zassert_equal(format_count, 2);
	zassert_equal(ts_av_config_bind(43, TS_AV_FMT_JPEG, 32, 16), TS_OK);
	expected_format = VIDEO_PIX_FMT_JPEG;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(format_count, 3);
	zassert_equal(ts_av_config_bind(43, TS_AV_FMT_PCM16, 16, 0), TS_OK);
	zassert_equal(allocated, 0);
	zassert_equal(stop_count, 3);
}

ZTEST(camera_contract, test_failed_rebind_preserves_previous_configuration)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	stop_failure = true;
	zassert_equal(ts_av_config_bind(43, TS_AV_FMT_RGB565, 32, 16), TS_E_IO);
	zassert_equal(allocated, CAMERA_BUFFERS);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	stop_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(len, FRAME_BYTES);
	zassert_equal(allocated, CAMERA_BUFFERS);
}

ZTEST(camera_contract, test_rebind_after_partial_restart_failure)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	forced_timeouts = 3;
	start_failure = true;
	for (unsigned int i = 0; i < 3; i++) {
		zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	}
	zassert_equal(stop_count, 1);
	zassert_false(stopped);
	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_PCM16, 16, 0), TS_OK);
	zassert_equal(stop_count, 2);
	zassert_equal(allocated, 0);
}

ZTEST(camera_contract, test_format_failure_and_invalid_binding)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	format_failure = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_IO);
	zassert_equal(allocation_count, 0);
	zassert_equal(len, 17);
	format_failure = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(ts_av_config_bind(43, TS_AV_FMT_RGB565, 0, 16), TS_E_PARAM);
	zassert_equal(stop_count, 0);
	allowed = false;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_E_PERM);
	zassert_equal(stop_count, 0);
	allowed = true;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
}

static void capture_worker(void *p1, void *p2, void *p3)
{
	ts_ctx_t ctx = {.app_id = 42};
	uint8_t frame[FRAME_BYTES];
	uint32_t len = 17;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	capture_result = ts_av_capture(ctx, frame, sizeof(frame), &len);
}

static void binding_worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	k_sem_give(&binding_entered);
	binding_result = ts_av_config_bind(43, TS_AV_FMT_RGB565, 32, 16);
	k_sem_give(&binding_finished);
}

ZTEST(camera_contract, test_rebind_waits_for_capture)
{
	ts_ctx_t ctx = {.app_id = 43};
	uint8_t frame[MAX_FRAME_BYTES];
	uint32_t len = 17;

	zassert_equal(ts_av_config_bind(42, TS_AV_FMT_RGB565, 16, 16), TS_OK);
	block_capture = true;
	k_thread_create(&capture_thread, capture_stack, K_THREAD_STACK_SIZEOF(capture_stack),
			capture_worker, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	zassert_equal(k_sem_take(&capture_entered, K_SECONDS(2)), 0);
	k_thread_create(&binding_thread, binding_stack, K_THREAD_STACK_SIZEOF(binding_stack),
			binding_worker, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	zassert_equal(k_sem_take(&binding_entered, K_SECONDS(2)), 0);
	zassert_equal(k_sem_take(&binding_finished, K_MSEC(10)), -EAGAIN);
	zassert_equal(stop_count, 0);
	zassert_equal(release_count, 0);
	k_sem_give(&capture_resume);
	zassert_equal(k_thread_join(&capture_thread, K_SECONDS(2)), 0);
	zassert_equal(k_thread_join(&binding_thread, K_SECONDS(2)), 0);
	zassert_equal(capture_result, TS_OK);
	zassert_equal(binding_result, TS_OK);
	zassert_equal(allocated, 0);
	expected_width = 32;
	reported_size = MAX_FRAME_BYTES;
	zassert_equal(ts_av_capture(ctx, frame, sizeof(frame), &len), TS_OK);
	zassert_equal(len, MAX_FRAME_BYTES);
}

ZTEST_SUITE(camera_contract, NULL, NULL, reset_test, NULL, NULL);
