// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#define BUFFER_COUNT 2U
#define WARMUP_FRAMES 90U
#define ENCODE_CHUNK 768U

static uint32_t adler32_bytes(const uint8_t *data, size_t size)
{
	uint32_t a = 1, b = 0;

	for (size_t i = 0; i < size; ++i) {
		a = (a + data[i]) % 65521U;
		b = (b + a) % 65521U;
	}
	return (b << 16) | a;
}

static int send_preview(const uint8_t *data, size_t size)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char encoded[(ENCODE_CHUNK / 3U) * 4U + 2U];

	for (size_t offset = 0; offset < size;) {
		size_t count = size - offset;
		size_t out = 0;

		if (count > ENCODE_CHUNK) {
			count = ENCODE_CHUNK;
		}
		for (size_t i = 0; i < count; i += 3U) {
			size_t left = count - i;
			uint32_t value = (uint32_t)data[offset + i] << 16;

			if (left > 1U) {
				value |= (uint32_t)data[offset + i + 1U] << 8;
			}
			if (left > 2U) {
				value |= data[offset + i + 2U];
			}
			encoded[out++] = alphabet[(value >> 18) & 63U];
			encoded[out++] = alphabet[(value >> 12) & 63U];
			encoded[out++] = left > 1U ? alphabet[(value >> 6) & 63U] : '=';
			encoded[out++] = left > 2U ? alphabet[value & 63U] : '=';
		}
		encoded[out++] = '\n';
		if (fwrite(encoded, 1, out, stdout) != out) {
			return -1;
		}
		offset += count;
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	return fflush(stdout);
}

void app_main(void)
{
	esp_ldo_channel_handle_t phy_ldo = NULL;
	esp_ldo_channel_config_t ldo = {.chan_id = 3, .voltage_mv = 2500};
	esp_video_init_csi_config_t csi = {
		.sccb_config =
			{
				.init_sccb = true,
				.i2c_config = {.port = 0, .scl_pin = 8, .sda_pin = 7},
				.freq = 100000,
			},
		.reset_pin = -1,
		.pwdn_pin = -1,
	};
	esp_video_init_config_t config = {.csi = &csi};
	struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
	struct v4l2_requestbuffers request = {
		.count = BUFFER_COUNT,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	void *mapped[BUFFER_COUNT] = {0};
	size_t mapped_size[BUFFER_COUNT] = {0};
	uint8_t *preview = NULL;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int fd = -1;
	bool streaming = false;
	esp_err_t err;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("P4CAM START sensor=SC2336 i2c=7/8\n");
	err = esp_ldo_acquire_channel(&ldo, &phy_ldo);
	if (err != ESP_OK) {
		printf("P4CAM FAIL ldo err=%s\n", esp_err_to_name(err));
		goto cleanup;
	}
	err = esp_video_init(&config);
	if (err != ESP_OK) {
		printf("P4CAM FAIL video_init err=%s\n", esp_err_to_name(err));
		goto cleanup;
	}
	fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
	if (fd < 0 || ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
		printf("P4CAM FAIL open_or_format errno=%d\n", errno);
		goto cleanup;
	}
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
	if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
		printf("P4CAM FAIL set_rgb565 errno=%d\n", errno);
		goto cleanup;
	}
	if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
		printf("P4CAM FAIL get_negotiated_format errno=%d\n", errno);
		goto cleanup;
	}
	uint32_t width = format.fmt.pix.width;
	uint32_t height = format.fmt.pix.height;
	uint32_t stride = format.fmt.pix.bytesperline;

	printf("P4CAM FORMAT width=%" PRIu32 " height=%" PRIu32 " stride=%" PRIu32 " size=%" PRIu32
	       " fourcc=%08" PRIx32 "\n",
	       width, height, stride, format.fmt.pix.sizeimage, format.fmt.pix.pixelformat);
	if (width < 2U || height < 2U || width > 1920U || height > 1080U || (width & 1U) ||
	    (height & 1U) || (stride && stride < width * 2U) ||
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
		printf("P4CAM FAIL unsupported_format\n");
		goto cleanup;
	}
	if (!stride) {
		stride = width * 2U;
		printf("P4CAM INFO esp_video_2_4_1_packed_stride=%" PRIu32 "\n", stride);
	}
	if (ioctl(fd, VIDIOC_REQBUFS, &request) != 0 || request.count < BUFFER_COUNT) {
		printf("P4CAM FAIL request_buffers errno=%d count=%" PRIu32 "\n", errno,
		       request.count);
		goto cleanup;
	}
	for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
		struct v4l2_buffer buffer = {
			.type = type,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};

		if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
			printf("P4CAM FAIL query index=%" PRIu32 " errno=%d\n", i, errno);
			goto cleanup;
		}
		mapped_size[i] = buffer.length;
		mapped[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				 buffer.m.offset);
		if (mapped[i] == MAP_FAILED || mapped[i] == NULL) {
			mapped[i] = NULL;
			printf("P4CAM FAIL mmap index=%" PRIu32 " errno=%d\n", i, errno);
			goto cleanup;
		}
		if (ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
			printf("P4CAM FAIL queue index=%" PRIu32 " errno=%d\n", i, errno);
			goto cleanup;
		}
	}
	if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
		printf("P4CAM FAIL stream_on errno=%d\n", errno);
		goto cleanup;
	}
	streaming = true;
	struct v4l2_buffer frame = {.type = type, .memory = V4L2_MEMORY_MMAP};

	for (unsigned int i = 0; i < WARMUP_FRAMES; ++i) {
		if (ioctl(fd, VIDIOC_DQBUF, &frame) != 0 || frame.index >= BUFFER_COUNT) {
			printf("P4CAM FAIL dequeue frame=%u errno=%d\n", i, errno);
			goto cleanup;
		}
		if (i + 1U != WARMUP_FRAMES && ioctl(fd, VIDIOC_QBUF, &frame) != 0) {
			printf("P4CAM FAIL requeue frame=%u errno=%d\n", i, errno);
			goto cleanup;
		}
	}
	printf("P4CAM CAPTURE frames=%u bytesused=%" PRIu32 " sequence=%" PRIu32 "\n",
	       WARMUP_FRAMES, frame.bytesused, frame.sequence);
	if (frame.bytesused < stride * height || mapped_size[frame.index] < stride * height) {
		printf("P4CAM FAIL frame_size used=%" PRIu32 " mapped=%u\n", frame.bytesused,
		       (unsigned int)mapped_size[frame.index]);
		goto cleanup;
	}
	if (ioctl(fd, VIDIOC_STREAMOFF, &type) != 0) {
		printf("P4CAM FAIL stream_off errno=%d\n", errno);
		goto cleanup;
	}
	streaming = false;
	size_t preview_size = (size_t)(width / 2U) * (height / 2U) * 2U;

	preview = heap_caps_malloc(preview_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!preview) {
		printf("P4CAM FAIL preview_allocation size=%u\n", (unsigned int)preview_size);
		goto cleanup;
	}
	const uint8_t *image = mapped[frame.index];
	size_t dest = 0;

	for (uint32_t y = 0; y < height; y += 2U) {
		for (uint32_t x = 0; x < width; x += 2U) {
			size_t source = (size_t)y * stride + x * 2U;

			preview[dest++] = image[source];
			preview[dest++] = image[source + 1U];
		}
	}
	uint32_t checksum = adler32_bytes(preview, preview_size);

	printf("P4CAM FRAME_BEGIN width=%" PRIu32 " height=%" PRIu32 " bytes=%u adler32=%08" PRIx32
	       " format=RGB565\n",
	       width / 2U, height / 2U, (unsigned int)preview_size, checksum);
	if (send_preview(preview, preview_size) != 0) {
		printf("P4CAM FAIL transport\n");
		goto cleanup;
	}
	printf("P4CAM FRAME_END adler32=%08" PRIx32 "\n", checksum);

cleanup:
	if (streaming && ioctl(fd, VIDIOC_STREAMOFF, &type) != 0) {
		printf("P4CAM FAIL cleanup_stream errno=%d\n", errno);
	}
	free(preview);
	for (unsigned int i = 0; i < BUFFER_COUNT; ++i) {
		if (mapped[i] && munmap(mapped[i], mapped_size[i]) != 0) {
			printf("P4CAM FAIL munmap index=%u errno=%d\n", i, errno);
		}
	}
	if (fd >= 0 && close(fd) != 0) {
		printf("P4CAM FAIL close errno=%d\n", errno);
	}
	printf("P4CAM END\n");
}
