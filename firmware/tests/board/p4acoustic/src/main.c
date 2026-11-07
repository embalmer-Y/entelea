// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/gpio_ll.h>
#include <rom/gpio.h>

#define CODEC_ADDRESS 0x18
#define AMP_PIN (53U - 32U)
#define FRAMES 256U
#define BLOCK_SIZE (FRAMES * 2U * sizeof(uint32_t))
#define PREFILL 4U
#define WARM_BLOCKS 32U
#define MEASURE_BLOCKS 32U
#define INITIAL_WARM_BLOCKS 320U
#define PHASE_BLOCKS (WARM_BLOCKS + MEASURE_BLOCKS)
#define TOTAL_BLOCKS (INITIAL_WARM_BLOCKS + 3U * PHASE_BLOCKS)

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
	uint32_t nonzero;
	uint32_t clipped;
	uint32_t valid_sign24;
	int64_t real[3];
	int64_t imag[3];
	uint32_t n;
};

static struct stats measures[3][2][3];
static uint32_t pin_high[5];
static uint32_t pin_edges[5];
static int amp_levels[3];
static uint32_t ws_hz;

static int measure_ws(uint32_t *frequency)
{
	uint64_t start = k_cycle_get_64();
	uint64_t first = 0, last = 0;
	uint32_t previous = gpio_ll_get_level(&GPIO, 10);
	unsigned int edges = 0;

	while (k_cyc_to_ms_floor64(k_cycle_get_64() - start) < 30U) {
		uint32_t level = gpio_ll_get_level(&GPIO, 10);

		if (level && !previous) {
			uint64_t now = k_cycle_get_64();

			if (edges == 0) {
				first = now;
			}
			if (++edges == 128) {
				last = now;
				break;
			}
		}
		previous = level;
	}
	if (edges != 128) {
		return -ETIMEDOUT;
	}
	uint64_t elapsed_ns = k_cyc_to_ns_floor64(last - first);

	if (elapsed_ns == 0) {
		return -ERANGE;
	}
	*frequency = (uint32_t)(127ULL * 1000000000ULL / elapsed_ns);
	return 0;
}

static int codec_write(uint8_t reg, uint8_t value)
{
	int ret = i2c_reg_write_byte(i2c, CODEC_ADDRESS, reg, value);

	if (ret != 0) {
		printk("P4AUDIO FAIL codec_write reg=%02x rc=%d\n", reg, ret);
	}
	return ret;
}

static int codec_init(void)
{
	static const uint8_t config[][2] = {
		{0x00, 0x00}, {0x00, 0x80}, {0x01, 0xbf}, {0x02, 0x10}, {0x03, 0x10}, {0x04, 0x10},
		{0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xff}, {0x09, 0x10}, {0x0a, 0x10},
		{0x0d, 0x01}, {0x0e, 0x02}, {0x12, 0x00}, {0x13, 0x10}, {0x14, 0x1a}, {0x15, 0x40},
		{0x16, 0x04}, {0x17, 0xc8}, {0x1b, 0x0a}, {0x1c, 0x6a}, {0x31, 0x00}, {0x32, 0x80},
		{0x37, 0x08}, {0x44, 0x08}, {0x45, 0x00},
	};
	uint8_t id1 = 0, id2 = 0, reg00 = 0;
	int ret = i2c_reg_read_byte(i2c, CODEC_ADDRESS, 0xfd, &id1);

	if (ret == 0) {
		ret = i2c_reg_read_byte(i2c, CODEC_ADDRESS, 0xfe, &id2);
	}
	if (ret != 0) {
		printk("P4AUDIO FAIL codec_id rc=%d\n", ret);
		return ret;
	}
	printk("P4AUDIO CODEC id=%02x%02x\n", id1, id2);
	if (id1 != 0x83 || id2 != 0x11) {
		return -ENODEV;
	}
	ret = codec_write(0x00, 0x1f);
	if (ret != 0) {
		return ret;
	}
	k_msleep(20);
	for (size_t i = 0; i < ARRAY_SIZE(config); ++i) {
		ret = codec_write(config[i][0], config[i][1]);
		if (ret != 0) {
			return ret;
		}
	}
	ret = i2c_reg_read_byte(i2c, CODEC_ADDRESS, 0x00, &reg00);
	if (ret != 0 || reg00 != 0x80) {
		printk("P4AUDIO FAIL codec_readback rc=%d reg00=%02x\n", ret, reg00);
		return ret != 0 ? ret : -EIO;
	}
	return 0;
}

static int enqueue(unsigned int block)
{
	uint32_t *samples;
	int ret = k_mem_slab_alloc(&tx_slab, (void **)&samples, K_MSEC(1000));

	if (ret != 0) {
		return ret;
	}
	for (unsigned int i = 0; i < FRAMES; ++i) {
		unsigned int phase = (block * FRAMES + i) % 16U;
		int16_t tone = 8 * sine32[phase * 2U];
		uint32_t word = (uint32_t)(uint16_t)tone << 16;

		samples[2 * i] = word;
		samples[2 * i + 1] = word;
	}
	ret = i2s_write(i2s, samples, BLOCK_SIZE);
	if (ret != 0) {
		k_mem_slab_free(&tx_slab, samples);
	}
	return ret;
}

static void accumulate(struct stats *s, int16_t value, uint32_t position)
{
	static const unsigned int steps[3] = {1, 2, 4};

	s->square += (int64_t)value * value;
	s->sum += value;
	s->nonzero += value != 0;
	for (unsigned int bin = 0; bin < 3; ++bin) {
		unsigned int p = (position * steps[bin]) % 32U;

		s->real[bin] += (int64_t)value * sine32[(p + 8U) % 32U];
		s->imag[bin] += (int64_t)value * sine32[p];
	}
	++s->n;
}

static uint64_t magnitude(const struct stats *s, unsigned int bin)
{
	int64_t re = s->real[bin], im = s->imag[bin];

	return ((uint64_t)(re < 0 ? -re : re) + (uint64_t)(im < 0 ? -im : im)) / (s->n * 1024ULL);
}

static int run_audio(void)
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
	int ret = i2s_configure(i2s, I2S_DIR_TX, &config);

	if (ret == 0) {
		config.mem_slab = &rx_slab;
		ret = i2s_configure(i2s, I2S_DIR_RX, &config);
	}
	if (ret != 0) {
		printk("P4AUDIO FAIL configure rc=%d\n", ret);
		return ret;
	}
	static const unsigned int pins[5] = {13, 12, 10, 9, 11};

	for (unsigned int p = 0; p < ARRAY_SIZE(pins); ++p) {
		rom_gpio_pad_input_enable(pins[p]);
	}
	for (unsigned int b = 0; b < PREFILL; ++b) {
		ret = enqueue(b);
		if (ret != 0) {
			printk("P4AUDIO FAIL prefill block=%u rc=%d\n", b, ret);
			goto cleanup;
		}
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (ret != 0) {
		printk("P4AUDIO FAIL start rc=%d\n", ret);
		goto cleanup;
	}
	for (unsigned int b = 0; b < TOTAL_BLOCKS; ++b) {
		uint32_t *samples;
		size_t len;
		unsigned int phase_block = b < INITIAL_WARM_BLOCKS ? 0 : b - INITIAL_WARM_BLOCKS;
		unsigned int phase = phase_block / PHASE_BLOCKS;

		if (b >= INITIAL_WARM_BLOCKS &&
		    (phase_block == PHASE_BLOCKS || phase_block == 2U * PHASE_BLOCKS)) {
			ret = gpio_pin_set(amp_gpio, AMP_PIN, phase_block == PHASE_BLOCKS);
			if (ret != 0) {
				printk("P4AUDIO FAIL amplifier phase=%u rc=%d\n", phase, ret);
				goto cleanup;
			}
			if (phase_block == 2U * PHASE_BLOCKS) {
				ret = i2c_reg_update_byte(i2c, CODEC_ADDRESS, 0x31, 0x60, 0x60);
				if (ret != 0) {
					printk("P4AUDIO FAIL codec_mute rc=%d\n", ret);
					goto cleanup;
				}
			}
		}
		if (b >= INITIAL_WARM_BLOCKS && phase_block % PHASE_BLOCKS == 1) {
			amp_levels[phase] = gpio_pin_get_raw(amp_gpio, AMP_PIN);
			if (amp_levels[phase] < 0 || amp_levels[phase] != (phase == 1 ? 1 : 0)) {
				printk("P4AUDIO FAIL amplifier_readback phase=%u level=%d\n", phase,
				       amp_levels[phase]);
				ret = -EIO;
				goto cleanup;
			}
		}
		ret = i2s_read(i2s, (void **)&samples, &len);
		if (ret != 0) {
			printk("P4AUDIO FAIL read block=%u rc=%d\n", b, ret);
			goto cleanup;
		}
		if (len != BLOCK_SIZE) {
			k_mem_slab_free(&rx_slab, samples);
			printk("P4AUDIO FAIL length block=%u len=%u\n", b, (unsigned int)len);
			ret = -EMSGSIZE;
			goto cleanup;
		}
		if (b == 2) {
			uint32_t previous[5] = {0};

			for (unsigned int n = 0; n < 4096; ++n) {
				for (unsigned int p = 0; p < ARRAY_SIZE(pins); ++p) {
					uint32_t level = gpio_ll_get_level(&GPIO, pins[p]);

					pin_high[p] += level;
					if (n != 0 && level != previous[p]) {
						++pin_edges[p];
					}
					previous[p] = level;
				}
			}
			ret = measure_ws(&ws_hz);
			if (ret != 0 || ws_hz < 15800 || ws_hz > 16200) {
				k_mem_slab_free(&rx_slab, samples);
				printk("P4AUDIO FAIL ws_clock hz=%u rc=%d\n", ws_hz, ret);
				ret = ret != 0 ? ret : -ERANGE;
				goto cleanup;
			}
		}
		if (b >= INITIAL_WARM_BLOCKS && phase_block % PHASE_BLOCKS >= WARM_BLOCKS) {
			for (unsigned int i = 0; i < FRAMES; ++i) {
				for (unsigned int ch = 0; ch < 2; ++ch) {
					uint32_t word = samples[2 * i + ch];
					int32_t scaled = (int32_t)word / 256;
					struct stats *mid = &measures[phase][ch][2];

					mid->valid_sign24 +=
						(word >> 24) == ((word & 0x00800000U) ? 0xffU : 0U);
					if (scaled > INT16_MAX) {
						scaled = INT16_MAX;
						++mid->clipped;
					} else if (scaled < INT16_MIN) {
						scaled = INT16_MIN;
						++mid->clipped;
					}

					accumulate(&measures[phase][ch][0], (int16_t)(word >> 16),
						   b * FRAMES + i);
					accumulate(&measures[phase][ch][1], (int16_t)word,
						   b * FRAMES + i);
					accumulate(mid, (int16_t)scaled, b * FRAMES + i);
				}
			}
		}
		k_mem_slab_free(&rx_slab, samples);
		ret = enqueue(PREFILL + b);
		if (ret != 0) {
			printk("P4AUDIO FAIL write block=%u rc=%d\n", b, ret);
			goto cleanup;
		}
	}
	ret = gpio_pin_set(amp_gpio, AMP_PIN, 0);
	if (ret != 0) {
		printk("P4AUDIO FAIL amplifier_off rc=%d\n", ret);
		goto cleanup;
	}
	ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	if (ret != 0) {
		printk("P4AUDIO FAIL drop rc=%d\n", ret);
		goto cleanup;
	}
	void *after;
	size_t after_len;

	ret = i2s_read(i2s, &after, &after_len);
	if (ret == 0) {
		k_mem_slab_free(&rx_slab, after);
		printk("P4AUDIO FAIL read_after_drop len=%u\n", (unsigned int)after_len);
		return -EBADMSG;
	}
	if (ret != -EAGAIN) {
		printk("P4AUDIO FAIL read_after_drop rc=%d\n", ret);
		return ret;
	}
	printk("P4AUDIO STOP read_after_drop_rc=%d\n", ret);
	printk("P4AUDIO WS_HZ %u edges=128 reference=zephyr_cycle_clock\n", ws_hz);
	printk("P4AUDIO AMP_LEVELS off=%d on=%d after=%d\n", amp_levels[0], amp_levels[1],
	       amp_levels[2]);
	for (unsigned int p = 0; p < ARRAY_SIZE(pins); ++p) {
		printk("P4AUDIO PIN pin=%u high=%u edges=%u samples=4096\n", pins[p], pin_high[p],
		       pin_edges[p]);
	}
	return 0;

cleanup: {
	int amp_ret = gpio_pin_set(amp_gpio, AMP_PIN, 0);
	int drop_ret = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);

	printk("P4AUDIO CLEANUP amp_rc=%d drop_rc=%d\n", amp_ret, drop_ret);
}
	return ret;
}

int main(void)
{
	int ret;
	bool acoustic = false, frequency = false;

	if (!device_is_ready(i2s) || !device_is_ready(i2c) || !device_is_ready(amp_gpio)) {
		printk("P4AUDIO FAIL device_not_ready\n");
		return 1;
	}
	ret = gpio_pin_configure(amp_gpio, AMP_PIN, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
	if (ret != 0) {
		printk("P4AUDIO FAIL amplifier_init rc=%d\n", ret);
		return 1;
	}
	ret = codec_init();
	if (ret == 0) {
		printk("P4AUDIO START rate=16000 stereo_slot=32 amp=8192 dac=80\n");
		ret = run_audio();
	}
	if (ret != 0) {
		printk("P4AUDIO END failure=%d\n", ret);
		return 1;
	}
	for (unsigned int ch = 0; ch < 2; ++ch) {
		for (unsigned int lane = 0; lane < 3; ++lane) {
			struct stats *base = &measures[0][ch][lane];
			struct stats *play = &measures[1][ch][lane];
			struct stats *after = &measures[2][ch][lane];
			uint64_t base_power = base->square / base->n;
			uint64_t play_power = play->square / play->n;
			uint64_t after_power = after->square / after->n;
			int64_t base_mean = base->sum / base->n;
			int64_t play_mean = play->sum / play->n;
			int64_t after_mean = after->sum / after->n;
			uint64_t base_ac = base_power - base_mean * base_mean;
			uint64_t play_ac = play_power - play_mean * play_mean;
			uint64_t after_ac = after_power - after_mean * after_mean;
			uint64_t f500 = magnitude(play, 0);
			uint64_t f1000 = magnitude(play, 1);
			uint64_t f2000 = magnitude(play, 2);
			uint64_t f1000_base = magnitude(base, 1);
			uint64_t f1000_after = magnitude(after, 1);

			printk("P4AUDIO STATS ch=%u lane=%s n=%u power_off=%" PRIu64
			       " power_on=%" PRIu64 " power_after=%" PRIu64 " f500=%" PRIu64
			       " f1000=%" PRIu64 " f2000=%" PRIu64 " f1000_off=%" PRIu64
			       " f1000_after=%" PRIu64 " ac_off=%" PRIu64 " ac_on=%" PRIu64
			       " ac_after=%" PRIu64 " nonzero=%u sign24=%u clipped=%u\n",
			       ch,
			       lane == 0   ? "high"
			       : lane == 1 ? "low"
					   : "signed24",
			       play->n, base_power, play_power, after_power, f500, f1000, f2000,
			       f1000_base, f1000_after, base_ac, play_ac, after_ac, play->nonzero,
			       play->valid_sign24, play->clipped);
			if ((lane != 2 || (play->valid_sign24 == play->n && play->clipped == 0)) &&
			    play_ac >= 100 && play_ac >= 4 * base_ac && play_ac >= 4 * after_ac) {
				acoustic = true;
				if (f1000 >= 10 && f1000 >= 2 * f500 && f1000 >= 2 * f2000) {
					frequency = true;
				}
			}
		}
	}
	if (!acoustic || !frequency) {
		printk("P4AUDIO FAIL acoustic=%d frequency=%d\n", acoustic, frequency);
		return 1;
	}
	printk("P4AUDIO PASS acoustic=1 frequency=1 stop=1\n");
	return 0;
}
