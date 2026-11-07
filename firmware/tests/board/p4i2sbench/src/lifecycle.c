// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <hal/gpio_ll.h>
#include <rom/gpio.h>
#include <soc/gpio_sig_map.h>

#define WORD_SIZE 32U
#define FRAMES_PER_BLOCK 256U
#define BLOCK_SIZE (FRAMES_PER_BLOCK * 2U * sizeof(uint32_t))
#define SLAB_BLOCKS 12U
#define PREFILL_BLOCKS 6U
#define BEFORE_BLOCKS 8U
#define AFTER_BLOCKS 4U
#define DATA_BASE 0x3000U
#define MAX_INITIAL_SKIP FRAMES_PER_BLOCK
#define CLOCK_QUIET_US 5000U
#define STOP_LIMIT_US 200000U

K_MEM_SLAB_DEFINE(rx_slab, BLOCK_SIZE, SLAB_BLOCKS, 64);
K_MEM_SLAB_DEFINE(tx_slab, BLOCK_SIZE, SLAB_BLOCKS, 64);

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));

struct sequence {
	uint32_t expected;
	uint32_t first;
	uint32_t checked;
	uint32_t skipped;
	bool aligned;
};

static int enqueue(unsigned int block)
{
	uint32_t *samples;
	int ret = k_mem_slab_alloc(&tx_slab, (void **)&samples, K_MSEC(250));

	if (ret != 0) {
		return ret;
	}
	for (unsigned int i = 0; i < FRAMES_PER_BLOCK; ++i) {
		uint16_t left = (uint16_t)(DATA_BASE + block * FRAMES_PER_BLOCK + i);

		samples[2 * i] = (uint32_t)left << 16;
		samples[2 * i + 1] = (uint32_t)(uint16_t)(left ^ 0xa55aU) << 16;
	}
	ret = i2s_write(i2s, samples, BLOCK_SIZE);
	if (ret != 0) {
		k_mem_slab_free(&tx_slab, samples);
	}
	return ret;
}

static int check_block(struct sequence *seq, const uint32_t *samples)
{
	for (unsigned int i = 0; i < FRAMES_PER_BLOCK; ++i) {
		uint16_t left = (uint16_t)(samples[2 * i] >> 16);
		uint16_t right = (uint16_t)(samples[2 * i + 1] >> 16);

		if (!seq->aligned) {
			if (left >= DATA_BASE &&
			    left < DATA_BASE + (PREFILL_BLOCKS + BEFORE_BLOCKS + AFTER_BLOCKS) *
						       FRAMES_PER_BLOCK &&
			    right == (uint16_t)(left ^ 0xa55aU)) {
				seq->expected = left;
				seq->first = left;
				seq->aligned = true;
			} else if (++seq->skipped <= MAX_INITIAL_SKIP) {
				continue;
			} else {
				printk("P4STOP FAIL alignment skipped=%u\n", seq->skipped);
				return -EBADMSG;
			}
		}
		if (left != (uint16_t)seq->expected ||
		    right != (uint16_t)((uint16_t)seq->expected ^ 0xa55aU) ||
		    (samples[2 * i] & 0xffffU) != 0U || (samples[2 * i + 1] & 0xffffU) != 0U) {
			printk("P4STOP FAIL data expected=%04x got=%04x/%04x checked=%u\n",
			       (uint16_t)seq->expected, left, right, seq->checked);
			return -EBADMSG;
		}
		++seq->expected;
		++seq->checked;
	}
	return 0;
}

static int receive(struct sequence *seq)
{
	void *samples;
	size_t len;
	int ret = i2s_read(i2s, &samples, &len);

	if (ret != 0) {
		return ret;
	}
	if (len != BLOCK_SIZE) {
		printk("P4STOP FAIL length=%u\n", (unsigned int)len);
		ret = -EMSGSIZE;
	} else {
		ret = check_block(seq, samples);
	}
	k_mem_slab_free(&rx_slab, samples);
	return ret;
}

static unsigned int ws_edges(unsigned int duration_us)
{
	uint64_t start = k_cycle_get_64();
	uint32_t last = gpio_ll_get_level(&GPIO, 10);
	unsigned int edges = 0;

	while (k_cyc_to_us_floor64(k_cycle_get_64() - start) < duration_us) {
		uint32_t level = gpio_ll_get_level(&GPIO, 10);

		if (level != last) {
			++edges;
			last = level;
		}
	}
	return edges;
}

static int wait_for_quiet(uint64_t start, uint32_t *elapsed_us)
{
	while (k_cyc_to_us_floor64(k_cycle_get_64() - start) < STOP_LIMIT_US) {
		if (ws_edges(CLOCK_QUIET_US) == 0U) {
			*elapsed_us = (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64() - start);
			return *elapsed_us <= STOP_LIMIT_US ? 0 : -ETIMEDOUT;
		}
	}
	return -ETIMEDOUT;
}

static int cycle(unsigned int run)
{
	struct sequence before = {0};
	struct sequence after = {0};
	unsigned int next_block = 0;
	unsigned int drained = 0;
	unsigned int free_tx;
	uint32_t stop_us = 0;
	uint64_t start;
	int ret;

	for (; next_block < PREFILL_BLOCKS; ++next_block) {
		ret = enqueue(next_block);
		if (ret != 0) {
			printk("P4STOP FAIL prefill run=%u rc=%d\n", run, ret);
			return ret;
		}
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret != 0) {
		printk("P4STOP FAIL start run=%u rc=%d\n", run, ret);
		return ret;
	}
	unsigned int edges = ws_edges(CLOCK_QUIET_US);

	if (edges < 30U) {
		printk("P4STOP FAIL running_clock run=%u edges=%u\n", run, edges);
		return -EIO;
	}
	for (unsigned int i = 0; i < BEFORE_BLOCKS; ++i) {
		ret = receive(&before);
		if (ret != 0) {
			printk("P4STOP FAIL before run=%u block=%u rc=%d\n", run, i, ret);
			return ret;
		}
		ret = enqueue(next_block++);
		if (ret != 0) {
			printk("P4STOP FAIL write_before run=%u block=%u rc=%d\n", run, i, ret);
			return ret;
		}
	}
	start = k_cycle_get_64();
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_STOP);
	if (ret != 0) {
		printk("P4STOP FAIL stop run=%u rc=%d\n", run, ret);
		return ret;
	}
	ret = wait_for_quiet(start, &stop_us);
	if (ret != 0) {
		printk("P4STOP FAIL clock_not_quiet run=%u rc=%d\n", run, ret);
		return ret;
	}
	do {
		ret = receive(&before);
		if (ret == 0) {
			++drained;
		}
	} while (ret == 0 && drained <= SLAB_BLOCKS);
	if (ret != -EAGAIN || drained == 0U || drained > SLAB_BLOCKS) {
		printk("P4STOP FAIL drain run=%u blocks=%u rc=%d\n", run, drained, ret);
		return -EIO;
	}
	if (ws_edges(CLOCK_QUIET_US) != 0U) {
		printk("P4STOP FAIL clock_restarted run=%u\n", run);
		return -EIO;
	}
	free_tx = k_mem_slab_num_free_get(&tx_slab);
	if (free_tx >= SLAB_BLOCKS) {
		printk("P4STOP FAIL tx_queue_not_preserved run=%u free=%u\n", run, free_tx);
		return -EIO;
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret != 0) {
		printk("P4STOP FAIL resume run=%u rc=%d free_tx=%u\n", run, ret, free_tx);
		return ret;
	}
	for (unsigned int i = 0; i < AFTER_BLOCKS; ++i) {
		ret = receive(&after);
		if (ret != 0) {
			printk("P4STOP FAIL after run=%u block=%u rc=%d\n", run, i, ret);
			return ret;
		}
		ret = enqueue(next_block++);
		if (ret != 0) {
			printk("P4STOP FAIL write_after run=%u block=%u rc=%d\n", run, i, ret);
			return ret;
		}
	}
	if (!after.aligned || after.skipped != 0U || after.first != before.expected) {
		printk("P4STOP FAIL resume_continuity run=%u expected=%04x got=%04x skipped=%u\n",
		       run, before.expected, after.first, after.skipped);
		return -EBADMSG;
	}
	if (before.checked < (BEFORE_BLOCKS - 1U) * FRAMES_PER_BLOCK ||
	    after.checked < (AFTER_BLOCKS - 1U) * FRAMES_PER_BLOCK) {
		printk("P4STOP FAIL checked run=%u before=%u after=%u\n", run, before.checked,
		       after.checked);
		return -EBADMSG;
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	if (ret != 0) {
		printk("P4STOP FAIL drop run=%u rc=%d\n", run, ret);
		return ret;
	}
	void *block;
	size_t len;

	ret = i2s_read(i2s, &block, &len);
	if (ret == 0) {
		k_mem_slab_free(&rx_slab, block);
		printk("P4STOP FAIL read_after_drop run=%u len=%u\n", run, (unsigned int)len);
		return -EIO;
	}
	if (ret != -EAGAIN || k_mem_slab_num_free_get(&tx_slab) != SLAB_BLOCKS ||
	    k_mem_slab_num_free_get(&rx_slab) != SLAB_BLOCKS) {
		printk("P4STOP FAIL cleanup run=%u rc=%d tx_free=%u rx_free=%u\n", run, ret,
		       k_mem_slab_num_free_get(&tx_slab), k_mem_slab_num_free_get(&rx_slab));
		return -EIO;
	}
	printk("P4STOP CYCLE_PASS run=%u clock_edges=%u stop_us=%u drained=%u "
	       "queued_tx=%u before=%u after=%u\n",
	       run, edges, stop_us, drained, SLAB_BLOCKS - free_tx, before.checked, after.checked);
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
		.timeout = 250,
	};
	int ret;

	if (!device_is_ready(i2s)) {
		printk("P4STOP FAIL device_not_ready\n");
		return 1;
	}
	ret = i2s_configure(i2s, I2S_DIR_TX, &config);
	if (ret == 0) {
		config.mem_slab = &rx_slab;
		ret = i2s_configure(i2s, I2S_DIR_RX, &config);
	}
	if (ret != 0) {
		printk("P4STOP FAIL configure rc=%d\n", ret);
		return 1;
	}
	rom_gpio_pad_input_enable(9);
	rom_gpio_pad_input_enable(10);
	rom_gpio_matrix_in(9, I2S0_I_SD_PAD_IN_IDX, false);
	printk("P4STOP START rate=16000 word=32 channels=2 stop_limit_us=%u\n", STOP_LIMIT_US);
	for (unsigned int run = 1; run <= 2; ++run) {
		ret = cycle(run);
		if (ret != 0) {
			int cleanup = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);

			printk("P4STOP END failure=%d cleanup=%d\n", ret, cleanup);
			return 1;
		}
	}
	printk("P4STOP PASS cycles=2\n");
	return 0;
}
