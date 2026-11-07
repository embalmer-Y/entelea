// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <rom/gpio.h>
#include <soc/gpio_sig_map.h>

#ifdef P04_I2S_WORD32
typedef uint32_t sample_t;
#define WORD_SIZE 32U
#define ENCODE(value) ((uint32_t)(value) << 16)
#define DECODE(value) ((uint16_t)((value) >> 16))
#define VALID_PADDING(value) (((value) & 0xffffU) == 0U)
#else
typedef uint16_t sample_t;
#define WORD_SIZE 16U
#define ENCODE(value) ((uint16_t)(value))
#define DECODE(value) ((uint16_t)(value))
#define VALID_PADDING(value) true
#endif

#define FRAMES_PER_BLOCK 256U
#define BLOCK_SIZE (FRAMES_PER_BLOCK * 2U * sizeof(sample_t))
#define PREFILL_BLOCKS 4U
#define RECEIVE_BLOCKS 48U
#define DATA_BASE 0x3000U

K_MEM_SLAB_DEFINE(rx_slab, BLOCK_SIZE, 12, 64);
K_MEM_SLAB_DEFINE(tx_slab, BLOCK_SIZE, 12, 64);

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));

static int enqueue(unsigned int block)
{
	sample_t *samples;
	int ret = k_mem_slab_alloc(&tx_slab, (void **)&samples, K_MSEC(1000));

	if (ret != 0) {
		return ret;
	}
	for (unsigned int i = 0; i < FRAMES_PER_BLOCK; ++i) {
		uint16_t left = (uint16_t)(DATA_BASE + block * FRAMES_PER_BLOCK + i);

		samples[2 * i] = ENCODE(left);
		samples[2 * i + 1] = ENCODE((uint16_t)(left ^ 0xa55aU));
	}
	ret = i2s_write(i2s, samples, BLOCK_SIZE);
	if (ret != 0) {
		k_mem_slab_free(&tx_slab, samples);
	}
	return ret;
}

static int cycle(unsigned int run)
{
	uint32_t expected = 0;
	uint32_t checked = 0;
	uint32_t first_frame = 0;
	uint32_t zero_pairs = 0;
	uint32_t reversed_pairs = 0;
	bool aligned = false;
	int ret;

	for (unsigned int b = 0; b < PREFILL_BLOCKS; ++b) {
		ret = enqueue(b);
		if (ret != 0) {
			printk("P4I2S FAIL prefill run=%u block=%u rc=%d\n", run, b, ret);
			return ret;
		}
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret != 0) {
		printk("P4I2S FAIL start run=%u rc=%d\n", run, ret);
		return ret;
	}
	for (unsigned int b = 0; b < RECEIVE_BLOCKS; ++b) {
		sample_t *samples;
		size_t len;

		ret = i2s_read(i2s, (void **)&samples, &len);
		if (ret != 0) {
			printk("P4I2S FAIL read run=%u block=%u rc=%d\n", run, b, ret);
			return ret;
		}
		if (len != BLOCK_SIZE) {
			printk("P4I2S FAIL length run=%u block=%u len=%u\n", run, b,
			       (unsigned int)len);
			k_mem_slab_free(&rx_slab, samples);
			return -EMSGSIZE;
		}
		for (unsigned int i = 0; i < FRAMES_PER_BLOCK; ++i) {
			uint16_t left = DECODE(samples[2 * i]);
			uint16_t right = DECODE(samples[2 * i + 1]);

			if (!aligned) {
				zero_pairs += left == 0 && right == 0;
				reversed_pairs +=
					right >= DATA_BASE &&
					right < DATA_BASE + PREFILL_BLOCKS * FRAMES_PER_BLOCK &&
					left == (uint16_t)(right ^ 0xa55aU);
				if (left >= DATA_BASE &&
				    left < DATA_BASE + PREFILL_BLOCKS * FRAMES_PER_BLOCK &&
				    right == (uint16_t)(left ^ 0xa55aU)) {
					expected = left - DATA_BASE;
					first_frame = expected;
					aligned = true;
				} else {
					continue;
				}
			}
			uint16_t wanted = (uint16_t)(DATA_BASE + expected);

			if (left != wanted || right != (uint16_t)(wanted ^ 0xa55aU) ||
			    !VALID_PADDING(samples[2 * i]) || !VALID_PADDING(samples[2 * i + 1])) {
				printk("P4I2S FAIL data run=%u block=%u frame=%u expected=%04x "
				       "got=%04x/%04x checked=%u\n",
				       run, b, i, wanted, left, right, checked);
				k_mem_slab_free(&rx_slab, samples);
				return -EBADMSG;
			}
			++expected;
			++checked;
		}
		k_mem_slab_free(&rx_slab, samples);
		ret = enqueue(PREFILL_BLOCKS + b);
		if (ret != 0) {
			printk("P4I2S FAIL write run=%u block=%u rc=%d\n", run, b, ret);
			return ret;
		}
	}
	if (!aligned || checked < (RECEIVE_BLOCKS - 1U) * FRAMES_PER_BLOCK) {
		printk("P4I2S FAIL alignment run=%u checked=%u zero_pairs=%u reversed_pairs=%u\n",
		       run, checked, zero_pairs, reversed_pairs);
		return -EBADMSG;
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	if (ret != 0) {
		printk("P4I2S FAIL drop run=%u rc=%d\n", run, ret);
		return ret;
	}
	void *after;
	size_t after_len;

	ret = i2s_read(i2s, &after, &after_len);
	if (ret == 0) {
		k_mem_slab_free(&rx_slab, after);
		printk("P4I2S FAIL read_after_drop run=%u len=%u\n", run, (unsigned int)after_len);
		return -EBADMSG;
	}
	if (ret != -EAGAIN) {
		printk("P4I2S FAIL read_after_drop run=%u rc=%d\n", run, ret);
		return ret;
	}
	printk("P4I2S CYCLE_PASS run=%u first_frame=%u checked=%u read_after_drop_rc=%d\n", run,
	       first_frame, checked, ret);
	return 0;
}

int main(void)
{
	struct i2s_config config = {
		.word_size = WORD_SIZE,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = 16000,
		.mem_slab = &tx_slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};
	int ret;

	if (!device_is_ready(i2s)) {
		printk("P4I2S FAIL device_not_ready\n");
		return 1;
	}
	ret = i2s_configure(i2s, I2S_DIR_TX, &config);
	if (ret == 0) {
		config.mem_slab = &rx_slab;
		ret = i2s_configure(i2s, I2S_DIR_RX, &config);
	}
	if (ret != 0) {
		printk("P4I2S FAIL configure rc=%d\n", ret);
		return 1;
	}
	rom_gpio_pad_input_enable(9);
	rom_gpio_matrix_in(9, I2S0_I_SD_PAD_IN_IDX, false);
	printk("P4I2S START internal_gpio9_loopback rate=16000 word=%u channels=2\n", WORD_SIZE);
	for (unsigned int run = 1; run <= 2; ++run) {
		ret = cycle(run);
		if (ret != 0) {
			int cleanup = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);

			printk("P4I2S END failure=%d cleanup=%d\n", ret, cleanup);
			return 1;
		}
	}
	printk("P4I2S PASS cycles=2\n");
	return 0;
}
