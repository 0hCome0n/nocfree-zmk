/*
 * Deep-sleep wake via expander INT (+ optional extra nRF pins).
 *
 * kscan is polled over I2C — expander key pins cannot wake SYSTEM OFF.
 * Stock ORs PCA9555 INT onto one MCU pin; we arm SENSE on that pin at PM
 * suspend (right before sys_poweroff).
 *
 * Before arming we READ every expander input port so the open-drain INT
 * line is released. If INT stays low (stuck press / bus fault), we refuse
 * deep sleep so the keyboard does not go radio-dead until a power cycle.
 *
 * UNBRICK paths (no case open):
 *   LEFT:  keypress on expander INT; mode-switch flip is NOT armed (races
 *          mode_switch.c). USB plug still wakes via activity (no SYSTEM OFF
 *          while USB powered).
 *   RIGHT: battery on/off power switch.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT nocfree_expander_int_wake

#define REG_INPUT_PORT0 0x00

struct expander_int_wake_config {
	const struct gpio_dt_spec *pins;
	size_t pin_count;
};

/* Read both input ports — clears PCA9555 INT latch (auto-increment). */
static void clear_expander_int_latches(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	static const uint8_t addrs[] = {0x20, 0x22, 0x24};
	uint8_t reg = REG_INPUT_PORT0;
	uint8_t data[2];

	if (!device_is_ready(bus)) {
		LOG_WRN("wake: i2c0 not ready — cannot clear expander INT");
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
		int err = i2c_write_read(bus, addrs[i], &reg, 1, data, sizeof(data));

		if (err) {
			LOG_WRN("wake: expander 0x%02x input read failed (%d)", addrs[i], err);
		}
	}
}

static int arm_one(const struct gpio_dt_spec *gpio, bool require_inactive)
{
	int err;
	int val;

	if (!gpio_is_ready_dt(gpio)) {
		LOG_ERR("wake GPIO not ready (%s.%u)",
			gpio->port ? gpio->port->name : "?", gpio->pin);
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(gpio, GPIO_INPUT);
	if (err) {
		LOG_ERR("wake configure input failed %s.%u: %d", gpio->port->name, gpio->pin,
			err);
		return err;
	}

	if (require_inactive) {
		val = gpio_pin_get_dt(gpio);
		if (val < 0) {
			LOG_WRN("wake pin read failed %s.%u: %d", gpio->port->name, gpio->pin,
				val);
			return val;
		}
		if (val != 0) {
			LOG_INF("wake skip %s.%u (already active — switch position)",
				gpio->port->name, gpio->pin);
			return -EBUSY;
		}
	}

	/*
	 * LEVEL_ACTIVE + ACTIVE_LOW in DT => physical low sense.
	 * nRF SYSTEM OFF wakes on DETECT; no IRQ callback required.
	 */
	err = gpio_pin_interrupt_configure_dt(gpio, GPIO_INT_LEVEL_ACTIVE);
	if (err) {
		LOG_ERR("wake sense arm failed %s.%u: %d", gpio->port->name, gpio->pin, err);
		return err;
	}

	LOG_INF("wake armed %s.%u", gpio->port->name, gpio->pin);
	return 0;
}

static int disarm_one(const struct gpio_dt_spec *gpio)
{
	if (!gpio_is_ready_dt(gpio)) {
		return -ENODEV;
	}
	return gpio_pin_interrupt_configure_dt(gpio, GPIO_INT_DISABLE);
}

static int expander_int_wake_arm(const struct device *dev)
{
	const struct expander_int_wake_config *cfg = dev->config;
	int armed = 0;
	int int_level;

	if (cfg->pin_count == 0) {
		LOG_ERR("no wake pins — refusing deep sleep");
		return -ENODEV;
	}

	/* Release open-drain INT so SENSE_LOW only fires on a real keypress. */
	clear_expander_int_latches();
	k_busy_wait(500);

	int_level = gpio_pin_get_dt(&cfg->pins[0]);
	if (int_level != 0) {
		/* Active (line low) OR a read error: either way we cannot prove
		 * the INT line is releasable, and SYSTEM OFF with a stuck-low
		 * SENSE line is an un-wakeable sleep (battery-pull recovery).
		 * A negative errno used to fall through the old `> 0` check and
		 * arm wake on an unverified line — fail closed instead. */
		LOG_ERR("expander INT not provably clear (%d) — abort deep sleep", int_level);
		return -EBUSY;
	}

	/* Pin 0 = expander INT: always try (key wake). */
	if (arm_one(&cfg->pins[0], false) == 0) {
		armed++;
	}

	/* Extra pins (mode switch): only if currently inactive. */
	for (size_t i = 1; i < cfg->pin_count; i++) {
		if (arm_one(&cfg->pins[i], true) == 0) {
			armed++;
		}
	}

	if (armed == 0) {
		LOG_ERR("armed 0 wake pins — abort deep sleep (stay on)");
		return -EIO;
	}

	LOG_INF("deep-sleep wake ready (%d pin(s)); keypress wakes", armed);
	return 0;
}

static int expander_int_wake_disarm(const struct device *dev)
{
	const struct expander_int_wake_config *cfg = dev->config;

	for (size_t i = 0; i < cfg->pin_count; i++) {
		(void)disarm_one(&cfg->pins[i]);
	}
	return 0;
}

static int expander_int_wake_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		return expander_int_wake_arm(dev);
	case PM_DEVICE_ACTION_RESUME:
		return expander_int_wake_disarm(dev);
	default:
		return -ENOTSUP;
	}
}

static int expander_int_wake_init(const struct device *dev)
{
	const struct expander_int_wake_config *cfg = dev->config;

	for (size_t i = 0; i < cfg->pin_count; i++) {
		if (!gpio_is_ready_dt(&cfg->pins[i])) {
			LOG_ERR("wake pin %u not ready at init", (unsigned)i);
			return -ENODEV;
		}
		(void)gpio_pin_configure_dt(&cfg->pins[i], GPIO_INPUT);
		(void)gpio_pin_interrupt_configure_dt(&cfg->pins[i], GPIO_INT_DISABLE);
	}

	LOG_INF("expander INT wake ready (%u pin(s))", (unsigned)cfg->pin_count);
	return 0;
}

#define WAKE_EXTRA_BY_IDX(idx, n) GPIO_DT_SPEC_INST_GET_BY_IDX(n, extra_gpios, idx)

#define EXPANDER_INT_WAKE_PINS(n)                                                                  \
	GPIO_DT_SPEC_INST_GET(n, int_gpios)                                                        \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, extra_gpios),                                         \
		    (, LISTIFY(DT_INST_PROP_LEN(n, extra_gpios), WAKE_EXTRA_BY_IDX, (, ), n)), ())

#define EXPANDER_INT_WAKE_DEFINE(n)                                                                \
	static const struct gpio_dt_spec expander_int_wake_pins_##n[] = {                          \
		EXPANDER_INT_WAKE_PINS(n)};                                                        \
	static const struct expander_int_wake_config expander_int_wake_cfg_##n = {                 \
		.pins = expander_int_wake_pins_##n,                                                \
		.pin_count = ARRAY_SIZE(expander_int_wake_pins_##n),                               \
	};                                                                                         \
	PM_DEVICE_DT_INST_DEFINE(n, expander_int_wake_pm_action);                                  \
	/* Init priority 65 is LOAD-BEARING (2026-08-14 review): the pm suspend  \
	 * walk runs in REVERSE init order, and our SUSPEND action does I2C      \
	 * (latch clear). At the old default (40, before i2c@50) the bus could   \
	 * already be suspended when the latch clear ran, silently leaving the   \
	 * PCA9555 INT latched -> arm aborts or sleeps unwakeable. 65 = after    \
	 * the expanders (60) so bus+expanders are still live for our suspend,   \
	 * before kscan (70) so scanning has stopped and cannot re-latch INT.    \
	 */                                                                                        \
	DEVICE_DT_INST_DEFINE(n, expander_int_wake_init, PM_DEVICE_DT_INST_GET(n), NULL,           \
			      &expander_int_wake_cfg_##n, POST_KERNEL, 65, NULL);

DT_INST_FOREACH_STATUS_OKAY(EXPANDER_INT_WAKE_DEFINE)
