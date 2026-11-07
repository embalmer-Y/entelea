// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define CODEC_ADDRESS 0x18
#define AMP_PIN (53U - 32U)
#define FRAMES 256U
#define BLOCK_SIZE (FRAMES * 2U * sizeof(uint32_t))
#define PREFILL 4U
#define WARM_BLOCKS 64U
#define WINDOW_BLOCKS 32U
#define WINDOWS 40U

K_MEM_SLAB_DEFINE(rx_slab, BLOCK_SIZE, 12, 64);
K_MEM_SLAB_DEFINE(tx_slab, BLOCK_SIZE, 12, 64);

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const amp_gpio = DEVICE_DT_GET(DT_NODELABEL(gpio1));

static const int16_t sine32[32] = {
	0,    200,   392,   569,   724,	 851,  946,  1004, 1024, 1004, 946,
	851,  724,   569,   392,   200,	 0,    -200, -392, -569, -724, -851,
	-946, -1004, -1024, -1004, -946, -851, -724, -569, -392, -200,
};

struct stats {
	uint64_t square;
	int64_t sum;
	int64_t real[2];
	int64_t imag[2];
	uint32_t n;
};

static int codec_init(void)
{
	static const uint8_t config[][2] = {
		{0x00, 0x00}, {0x00, 0x80}, {0x01, 0xbf}, {0x02, 0x10}, {0x03, 0x10}, {0x04, 0x10},
		{0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xff}, {0x09, 0x10}, {0x0a, 0x10},
		{0x0d, 0x01}, {0x0e, 0x02}, {0x12, 0x00}, {0x13, 0x10}, {0x14, 0x1a}, {0x15, 0x40},
		{0x16, 0x04}, {0x17, 0xc8}, {0x1b, 0x0a}, {0x1c, 0x6a}, {0x31, 0x60}, {0x32, 0x80},
		{0x37, 0x08}, {0x44, 0x08}, {0x45, 0x00},
	};
	uint8_t id1 = 0, id2 = 0, mute = 0;
	int ret = i2c_reg_read_byte(i2c, CODEC_ADDRESS, 0xfd, &id1);

	if (!ret)
		ret = i2c_reg_read_byte(i2c, CODEC_ADDRESS, 0xfe, &id2);
	if (ret || id1 != 0x83 || id2 != 0x11) {
		printk("P4MIC FAIL codec_id=%02x%02x rc=%d\n", id1, id2, ret);
		return ret ? ret : -ENODEV;
	}
	ret = i2c_reg_write_byte(i2c, CODEC_ADDRESS, 0x00, 0x1f);
	if (ret)
		return ret;
	k_msleep(20);
	for (size_t i = 0; i < ARRAY_SIZE(config); ++i) {
		ret = i2c_reg_write_byte(i2c, CODEC_ADDRESS, config[i][0], config[i][1]);
		if (ret) {
			printk("P4MIC FAIL codec_reg=%02x rc=%d\n", config[i][0], ret);
			return ret;
		}
	}
	ret = i2c_reg_read_byte(i2c, CODEC_ADDRESS, 0x31, &mute);
	printk("P4MIC CODEC id=%02x%02x mute=%02x rc=%d\n", id1, id2, mute, ret);
	if (ret || (mute & 0x60) != 0x60)
		return ret ? ret : -EIO;
	return 0;
}

static int enqueue_silence(void)
{
	uint32_t *samples;
	int ret = k_mem_slab_alloc(&tx_slab, (void **)&samples, K_MSEC(1000));

	if (ret)
		return ret;
	memset(samples, 0, BLOCK_SIZE);
	ret = i2s_write(i2s, samples, BLOCK_SIZE);
	if (ret)
		k_mem_slab_free(&tx_slab, samples);
	return ret;
}

static void accumulate(struct stats *s, int16_t sample, uint32_t position)
{
	static const unsigned int steps[2] = {2, 4};

	s->square += (int64_t)sample * sample;
	s->sum += sample;
	for (unsigned int bin = 0; bin < 2; ++bin) {
		unsigned int phase = (position * steps[bin]) % 32U;

		s->real[bin] += (int64_t)sample * sine32[(phase + 8U) % 32U];
		s->imag[bin] += (int64_t)sample * sine32[phase];
	}
	s->n++;
}

static uint64_t magnitude(const struct stats *s, unsigned int bin)
{
	int64_t re = s->real[bin], im = s->imag[bin];

	return ((uint64_t)(re < 0 ? -re : re) + (uint64_t)(im < 0 ? -im : im)) / (s->n * 1024ULL);
}

int main(void)
{
	struct i2s_config config = {
		.word_size = 32,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = 16000,
		.mem_slab = &tx_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};
	struct stats windows[2] = {0};
	bool started = false;
	int ret = 0;

	if (!device_is_ready(i2s) || !device_is_ready(i2c) || !device_is_ready(amp_gpio)) {
		printk("P4MIC FAIL device_not_ready\n");
		return 1;
	}
	ret = gpio_pin_configure(amp_gpio, AMP_PIN, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
	if (ret || (ret = codec_init()) != 0)
		goto cleanup;
	ret = gpio_pin_get_raw(amp_gpio, AMP_PIN);
	if (ret != 0) {
		printk("P4MIC FAIL amp_off_readback=%d\n", ret);
		ret = -EIO;
		goto cleanup;
	}
	ret = i2s_configure(i2s, I2S_DIR_TX, &config);
	if (!ret) {
		config.mem_slab = &rx_slab;
		ret = i2s_configure(i2s, I2S_DIR_RX, &config);
	}
	if (ret)
		goto cleanup;
	for (unsigned int i = 0; i < PREFILL; ++i) {
		ret = enqueue_silence();
		if (ret)
			goto cleanup;
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret)
		goto cleanup;
	started = true;
	for (unsigned int block = 0; block < WARM_BLOCKS + WINDOWS * WINDOW_BLOCKS; ++block) {
		uint32_t *samples;
		size_t len;

		if (block == WARM_BLOCKS)
			printk("P4MIC READY rate=16000 amp=0 dac_muted=1 windows=%u\n", WINDOWS);
		ret = i2s_read(i2s, (void **)&samples, &len);
		if (ret)
			goto cleanup;
		if (len != BLOCK_SIZE) {
			k_mem_slab_free(&rx_slab, samples);
			ret = -EMSGSIZE;
			goto cleanup;
		}
		if (block >= WARM_BLOCKS) {
			for (unsigned int i = 0; i < FRAMES; ++i)
				for (unsigned int ch = 0; ch < 2; ++ch)
					accumulate(&windows[ch],
						   (int16_t)(samples[2U * i + ch] >> 16),
						   block * FRAMES + i);
		}
		k_mem_slab_free(&rx_slab, samples);
		ret = enqueue_silence();
		if (ret)
			goto cleanup;
		if (block >= WARM_BLOCKS && (block - WARM_BLOCKS + 1U) % WINDOW_BLOCKS == 0) {
			unsigned int window = (block - WARM_BLOCKS) / WINDOW_BLOCKS;

			for (unsigned int ch = 0; ch < 2; ++ch) {
				struct stats *s = &windows[ch];
				int64_t mean = s->sum / s->n;
				uint64_t ac = s->square / s->n - mean * mean;

				printk("P4MIC WINDOW i=%u ch=%u n=%u ac=%" PRIu64 " f1000=%" PRIu64
				       " f2000=%" PRIu64 "\n",
				       window, ch, s->n, ac, magnitude(s, 0), magnitude(s, 1));
			}
			memset(windows, 0, sizeof(windows));
		}
	}
cleanup: {
	int amp_ret = gpio_pin_set(amp_gpio, AMP_PIN, 0);
	int drop_ret = started ? i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP) : 0;

	printk("P4MIC END rc=%d amp_rc=%d drop_rc=%d\n", ret, amp_ret, drop_ret);
	return ret || amp_ret || drop_ret ? 1 : 0;
}
}
