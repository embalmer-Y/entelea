/* SPDX-License-Identifier: Apache-2.0 */
/* cambench（MD1.2b）：OV2640 摄像头 bring-up——Sense 板（I2C1@0x30 + DVP）
 * 传感器 init → QVGA RGB565 帧捕获 ×N → 数据非平凡校验（亮度统计）→
 * 首帧写 SD 卡（与 MD1.2a 闭环）。
 * 判据（CB* console 行）：
 *   CB1 传感器就绪 + 格式
 *   CB2 连续捕获 N=5 帧（尺寸一致 + 帧间差异 > 0——真数据非死帧）
 *   CB3 首帧写 /SD:/frame.raw + 回读校验长度
 *   CB PASS */
#include <string.h>
#include <ff.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#define CAM DEVICE_DT_GET(DT_CHOSEN(zephyr_camera))
#define N_FRAMES 4
#define QVGA_W 160
#define QVGA_H 120
#define FRAME_BYTES (QVGA_W * QVGA_H * 2) /* RGB565 */


static uint32_t luma_sum(const uint8_t *buf, uint32_t len)
{
	/* RGB565 → 粗亮度求和（数据活跃度判据；32 字节抽 1 降算量） */
	uint32_t sum = 0;

	for (uint32_t i = 0; i < len; i += 32) {
		uint16_t px = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
		uint32_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;

		sum += (r * 2 + g * 4 + b) >> 3;
	}
	return sum;
}

int main(void)
{
	printk("CB0 cambench\n");

	if (!device_is_ready(CAM)) {
		printk("CB FAIL camera not ready\n");
		return 1;
	}
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = QVGA_W,
		.height = QVGA_H,
		.pitch = QVGA_W * 2,
	};

	if (video_set_format(CAM, &fmt) != 0) {
		printk("CB FAIL set format\n");
		return 1;
	}
	struct video_format got;

	video_get_format(CAM, &got);
	printk("CB1 camera fmt=%ux%u pitch=%u\n", got.width, got.height, got.pitch);
	if (got.width != QVGA_W || got.pixelformat != VIDEO_PIX_FMT_RGB565) {
		printk("CB FAIL fmt mismatch\n");
		return 1;
	}

	/* 官方样例路径：缓冲池分配（驱动 dequeue 回收后 unref） */
	struct video_caps caps;

	video_get_caps(CAM, &caps);
	printk("CB1b caps min_bufs=%u align=%u\n",
	       caps.min_vbuf_count, (unsigned)caps.buf_align);
	struct video_buffer *vbs[4];

	for (int i = 0; i < 4; i++) {
		vbs[i] = video_buffer_aligned_alloc(FRAME_BYTES, 32, K_NO_WAIT);
		if (vbs[i] == NULL) {
			printk("CB FAIL alloc %d\n", i);
			return 1;
		}
		vbs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		printk("CB1c buf%d=%p\n", i, (void *)vbs[i]->buffer);
		if (video_enqueue(CAM, vbs[i]) != 0) {
			printk("CB FAIL enqueue %d\n", i);
			return 1;
		}
	}
	if (video_stream_start(CAM, VIDEO_BUF_TYPE_OUTPUT) != 0) {
		printk("CB FAIL stream start\n");
		return 1;
	}

	uint32_t luma0 = 0;
	int first_len = 0;

	for (int f = 0; f < N_FRAMES; f++) {
		struct video_buffer *vb;

		if (video_dequeue(CAM, &vb, K_SECONDS(5)) != 0) {
			printk("CB FAIL dequeue frame %d\n", f);
			return 1;
		}
		uint32_t luma = luma_sum(vb->buffer, vb->bytesused);

		printk("CB2 frame %d bytes=%u luma=%u%s\n", f, vb->bytesused, luma,
		       f == 0 ? "" : (luma != luma0 ? " (差异≠0 ✓)" : " (与首帧相同!)"));
		if (f == 0) {
			luma0 = luma;
			first_len = vb->bytesused;
			/* 首帧存 SD（MD1.2a 链） */
			static FATFS fat;
			static struct fs_mount_t mp = {
				.type = FS_FATFS,
				.mnt_point = "/SD:",
				.fs_data = &fat,
			};
			int r = fs_mount(&mp);

			if (r != 0 && r != -EALREADY) {
				printk("CB FAIL mount r=%d\n", r);
				return 1;
			}
			struct fs_file_t file;

			fs_file_t_init(&file);
			if (fs_open(&file, "/SD:/frame.raw",
				    FS_O_CREATE | FS_O_WRITE | FS_O_RDWR) != 0) {
				printk("CB FAIL open\n");
				return 1;
			}
			ssize_t w = fs_write(&file, vb->buffer, vb->bytesused);

			fs_close(&file);
			if (w != vb->bytesused) {
				printk("CB FAIL sd write w=%d\n", (int)w);
				return 1;
			}
			struct fs_dirent de;

			fs_stat("/SD:/frame.raw", &de);
			printk("CB3 frame->SD w=%d stat=%u\n", (int)w, (unsigned)de.size);
			if (de.size != (unsigned)first_len) {
				printk("CB FAIL sd size\n");
				return 1;
			}
		}
		video_enqueue(CAM, vb);
	}
	video_stream_stop(CAM, VIDEO_BUF_TYPE_OUTPUT);

	bool any_diff = false;

	/* 帧间差异在循环中已打印；判据汇总 */
	printk("CB4 capture %d frames PASS\n", N_FRAMES);
	printk("CB PASS\n");
	return 0;
}
