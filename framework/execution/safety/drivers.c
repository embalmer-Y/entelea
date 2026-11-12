/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Driver dispatch (migrated from the old safety/driver_dispatch.c; R02
 * contract: framework/execution/safety/contract.md). This is the **only**
 * file in the whole tree that calls Zephyr output drivers (guarded by the
 * build-support/source-policy.json gate; the approved write path checked by
 * the architecture mechanical check). Write functions depend only on
 * registration-time frozen data (reentrant, lock-free). Failures go to
 * counters, since void write functions cannot report success or failure.
 * Changes from the old implementation: the generic GPIO input backend (used
 * by hal/input) is not migrated, but the estop input is kept. Making the
 * estop edge configuration prov-driven is deferred to R03 (R02 fixes the
 * default rising edge).
 */
#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <entelea/safety.h>
#include "internal.h"

static struct entelea_ch_slot *
slot_of(const entelea_out_channel_t *ch);

static void
sim_record(struct entelea_ch_slot *s, const entelea_out_value_t *v)
{
	if (s == NULL) {
		return;
	}
	s->sim_rec.ring[s->sim_rec.head] = (entelea_write_rec_t) {
		.t_ms = entelea_safety_now_ms(),
		.value_u = entelea_value_encode(s->desc->kind, *v),
	};
	s->sim_rec.head = (s->sim_rec.head + 1) % ENTELEA_SAFETY_SIM_REC_PER_CH;
	if (s->sim_rec.count < ENTELEA_SAFETY_SIM_REC_PER_CH) {
		s->sim_rec.count++;
	}
}

static int
sim_write(const entelea_out_channel_t *ch, const entelea_out_value_t *v)
{
	/* Stub path: the hardware backend also records first (golden
	 * continuity) before writing the register. The stub itself never
	 * fails; backend failure semantics come from return values on the
	 * hardware path (ztest injects a fake PWM device). */
	sim_record(slot_of(ch), v);
	return 0;
}

static int
sim_read(const entelea_out_channel_t *ch, entelea_out_value_t *out)
{
	struct entelea_ch_slot *s = slot_of(ch);

	if (s == NULL) {
		return -1;
	}
	*out = s->shadow;
	return 0;
}

/* ---- Hardware GPIO output backend (board-level replacement point) -----------
 * DT binding: zephyr,user node (uid = channel uid string match; io-gpios =
 * output pin). Write path = sim record + real register write; a uid mismatch
 * still records only. init is called explicitly by board assembly
 * (entelea_drv_gpio_init). A missing DT property yields -ENODEV
 * (configuration errors are reported as they are). */
#if defined(ENTELEA_DRV_GPIO) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), uid)
static const struct gpio_dt_spec real_io =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), io_gpios);
static const char *const real_io_uid = DT_PROP(DT_PATH(zephyr_user), uid);
static bool real_io_ready;

int
entelea_drv_gpio_init(void)
{
	if (!gpio_is_ready_dt(&real_io)) {
		return -EIO;
	}
	if (gpio_pin_configure_dt(&real_io, GPIO_OUTPUT_INACTIVE) != 0) {
		return -EIO;
	}
	real_io_ready = true;
	return 0;
}

static int
gpio_real_write(const entelea_out_channel_t *ch, const entelea_out_value_t *v)
{
	if (strcmp(ch->uid, real_io_uid) == 0) {
		/* Bound channel: a backend that is not ready (not
		 * initialized, init failed, or device not ready) is a
		 * configuration or device fault. Fail explicitly; never
		 * silently degrade to software success (second-round review
		 * finding 2). */
		if (!real_io_ready) {
			return -ENODEV;
		}
		int rc = gpio_pin_set_dt(&real_io, v->b); /* Propagate error */

		if (rc != 0) {
			return rc; /* Not recorded: ring = executed writes */
		}
	}
	/* Unbound channel = pure software channel (A3, owner-approved): a
	 * successful write only attests a software-level fact. */
	sim_record(slot_of(ch), v);
	return 0;
}

#define ENTELEA_GPIO_WRITE gpio_real_write
#else /* Disabled or no DT binding: stub path (init reports error) */
int
entelea_drv_gpio_init(void)
{
	return -ENODEV;
}

#define ENTELEA_GPIO_WRITE sim_write
#endif

/* ---- Hardware PWM output backend (board-level replacement point) ------------
 * DT binding: zephyr,user node (pwm-uid = channel uid; pwms three-cell
 * specifier). Write path = sim record + pwm_set (value range: low 16 bits of
 * u = permille [0,1000], high 16 bits = hz/100). The only write-path call
 * site is pwm_set, restricted to this file. Failures go to the pwm_err
 * counter (observation surface; incremented by the single lock-free
 * writer). */
#if defined(ENTELEA_DRV_PWM) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwm_uid)
#include <zephyr/drivers/pwm.h>
static const struct pwm_dt_spec real_pwm =
	PWM_DT_SPEC_GET(DT_PATH(zephyr_user));
static const char *const real_pwm_uid = DT_PROP(DT_PATH(zephyr_user), pwm_uid);
static bool real_pwm_ready;
static uint32_t pwm_err;

int
entelea_drv_pwm_init(void)
{
	if (!device_is_ready(real_pwm.dev)) {
		return -ENODEV;
	}
	real_pwm_ready = true;
	return 0;
}

uint32_t
entelea_drv_pwm_err_count(void)
{
	return pwm_err;
}

/* PWM packing decode (contract): low 16 bits of u = permille [0,1000],
 * high 16 bits = hz/100. u==0 (hz==0 and permille==0) means stop: duty 0
 * (fixed 1 s period, pulse width 0), guaranteeing that a zero value
 * physically stops the output in fault and safe states (defect 2).
 * hz==0 with permille>0, or permille>1000, is an illegal encoding. */
static int
pwm_decode(uint32_t u, uint32_t *period_ns, uint32_t *pulse_ns)
{
	uint32_t hz = (u >> 16) * 100U;
	uint32_t permille = u & 0xFFFFU;

	if (permille > 1000U) {
		return -EINVAL;
	}
	if (hz == 0U) {
		if (permille != 0U) {
			return -EINVAL;
		}
		*period_ns = 1000000000U;
		*pulse_ns = 0U;
		return 0;
	}
	uint32_t period = 1000000000U / hz;

	*period_ns = period;
	*pulse_ns = (uint32_t) ((uint64_t) period * permille / 1000U);
	return 0;
}

static int
pwm_real_write(const entelea_out_channel_t *ch, const entelea_out_value_t *v)
{
	if (strcmp(ch->uid, real_pwm_uid) == 0) {
		/* Bound channel: backend not ready = explicit failure
		 * (second-round review finding 2). */
		if (!real_pwm_ready) {
			pwm_err++;
			return -ENODEV;
		}
		uint32_t period_ns;
		uint32_t pulse_ns;

		if (pwm_decode(v->u, &period_ns, &pulse_ns) != 0) {
			pwm_err++;
			return -EINVAL; /* Out of range: explicit failure */
		}
		if (pwm_set(real_pwm.dev, real_pwm.channel, period_ns, pulse_ns,
			    real_pwm.flags) != 0) {
			pwm_err++; /* Backend failure: commit not OK */
			return -EIO; /* Not recorded: ring = executed writes */
		}
	}
	/* Unbound channel = pure software channel (A3): success attests only
	 * a software-level fact. */
	sim_record(slot_of(ch), v);
	return 0;
}

#define ENTELEA_PWM_WRITE pwm_real_write
#else
int
entelea_drv_pwm_init(void)
{
	return -ENODEV;
}

uint32_t
entelea_drv_pwm_err_count(void)
{
	return 0;
}

#define ENTELEA_PWM_WRITE sim_write
#endif

const entelea_driver_ops_t entelea_drivers[ENTELEA_CH_KIND_COUNT] = {
	[ENTELEA_CH_GPIO] = { ENTELEA_GPIO_WRITE, sim_read },
	[ENTELEA_CH_PWM] = { ENTELEA_PWM_WRITE, sim_read },
	[ENTELEA_CH_POWER] = { sim_write, sim_read },
};

size_t
entelea_driver_sim_writes(const char *uid, entelea_write_rec_t *out, size_t max)
{
	int i = entelea_ch_find(uid);

	if (i < 0) {
		return 0;
	}
	struct entelea_ch_slot *s = &entelea_ch_table[i];
	size_t n = s->sim_rec.count < max ? s->sim_rec.count : max;

	for (size_t k = 0; k < n; k++) {
		size_t idx = (s->sim_rec.head + ENTELEA_SAFETY_SIM_REC_PER_CH -
			      s->sim_rec.count + k) %
			     ENTELEA_SAFETY_SIM_REC_PER_CH;

		out[k] = s->sim_rec.ring[idx];
	}
	return n;
}

/* estop DT binding and ISR glue. Binding mechanism (first hardware landing
 * on r14): the named property estop-gpios on /zephyr,user (type inferred as
 * phandle-array, so no custom binding is needed; the earlier aliases
 * ts-estop-gpio approach did not generate a bare gpios macro in v4.5,
 * since nodes without a compatible emit no phandle-array properties).
 * Boards without the property (native_sim/tests) are a no-op. */
#define ESTOP_GPIO_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(ESTOP_GPIO_NODE, estop_gpios)
static const struct gpio_dt_spec estop_spec =
	GPIO_DT_SPEC_GET(ESTOP_GPIO_NODE, estop_gpios);
static struct gpio_callback estop_cb;

/* estop ISR: go straight to the safe state, without protocol stack or
 * scheduler queuing (call-graph discipline: see force.c). */
static void
estop_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	entelea_safety_force_all_fault();
}
#endif

entelea_result_t
entelea_safety_estop_init(void)
{
#if DT_NODE_HAS_PROP(ESTOP_GPIO_NODE, estop_gpios)
	if (!gpio_is_ready_dt(&estop_spec)) {
		return ENTELEA_E_IO;
	}
	/* Configure the input with pull-up before the interrupt (added before
	 * the first r14 hardware binding: dt_flags carries the overlay's
	 * ACTIVE_LOW|PULL_UP; enabling interrupts on an unconfigured input is
	 * undefined on some drivers). */
	if (gpio_pin_configure_dt(&estop_spec, GPIO_INPUT) != 0) {
		return ENTELEA_E_IO;
	}
	gpio_init_callback(&estop_cb, estop_isr, BIT(estop_spec.pin));
	if (gpio_add_callback_dt(&estop_spec, &estop_cb) != 0) {
		return ENTELEA_E_IO;
	}
	/* Trigger edge: R02 fixes the default to press edge (logical rising);
	 * provider-driven configuration is deferred to R03. The pure
	 * edge-mapping function is exposed as entelea_safety_estop_edge_of
	 * for test guards.
	 * r14 hardware evidence (a GPIO20 press did not trigger): Zephyr
	 * interrupt flags interpret physical levels by default. Only an
	 * explicit GPIO_INT_LEVELS_LOGICAL applies the pin's ACTIVE_LOW
	 * inversion (gpio.h z_impl inverts only the LEVELS_LOGICAL case).
	 * So the logical edge is translated into a physical edge per the
	 * spec's ACTIVE_LOW (press = physical falling); EDGE_BOTH is
	 * symmetric and is not inverted. */
	{
		int trig = entelea_safety_estop_edge_of(0);

		if ((estop_spec.dt_flags & GPIO_ACTIVE_LOW) != 0 &&
		    trig == GPIO_INT_EDGE_RISING) {
			trig = GPIO_INT_EDGE_FALLING;
		}
		if (gpio_pin_interrupt_configure_dt(&estop_spec, trig) != 0) {
			return ENTELEA_E_IO;
		}
	}
#endif
	return ENTELEA_OK; /* No-op without node; production must provide */
}

/* Raw estop pin level (0/1; -1 = unbound), for wiring diagnostics and
 * field observation. */
int
entelea_safety_estop_level_raw(void)
{
#if DT_NODE_HAS_PROP(ESTOP_GPIO_NODE, estop_gpios)
	return gpio_pin_get_raw(estop_spec.port, estop_spec.pin);
#else
	return -1;
#endif
}

/* estop edge mapping: **logical** edge semantics (0 = rising, default,
 * i.e. press edge; 1 = falling, i.e. release edge; 2 = both; unknown values
 * fail safe to rising). The physical-edge translation happens at the
 * drivers.c setup point (r14: under ACTIVE_LOW, logical rising = physical
 * falling). */
int
entelea_safety_estop_edge_of(uint8_t flags)
{
	switch (flags) {
	case 1:
		return GPIO_INT_EDGE_FALLING;
	case 2:
		return GPIO_INT_EDGE_BOTH;
	case 0:
	default:
		return GPIO_INT_EDGE_RISING;
	}
}

static struct entelea_ch_slot *
slot_of(const entelea_out_channel_t *ch)
{
	if (ch == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < entelea_ch_count; i++) {
		if (entelea_ch_table[i].desc == ch) {
			return &entelea_ch_table[i];
		}
	}
	return NULL;
}

#ifdef ENTELEA_TEST
void
entelea_drv_test_reset(void)
{
	/* Test isolation: reset backend-ready flags and failure counters
	 * (paired with the not-ready negative test). */
#if defined(ENTELEA_DRV_GPIO) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), uid)
	real_io_ready = false;
#endif
#if defined(ENTELEA_DRV_PWM) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwm_uid)
	real_pwm_ready = false;
	pwm_err = 0;
#endif
}
#endif
