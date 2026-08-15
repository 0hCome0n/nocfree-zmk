/*
 * Typical split battery UX: host BAS shows min(left, right).
 *
 * Left samples its pack; right reports over split when
 * CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING is on. Windows only
 * shows one bar — report the weaker half so either side low is visible.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static void apply_min_bas(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(bas_min_work, apply_min_bas);

static void apply_min_bas(struct k_work *work)
{
	ARG_UNUSED(work);

	uint8_t level = zmk_battery_state_of_charge();

	for (uint8_t i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
		uint8_t periph = 0;
		int rc = zmk_split_central_get_peripheral_battery_level(i, &periph);

		/* rc is 0 even for a never-connected peripheral (the vendored
		 * central just reads its level array, statically zero) — this
		 * function CANNOT distinguish offline from reported-0%. Treat 0
		 * as unknown, like ZMK's own BAS proxy does: otherwise every
		 * boot reports min(central, 0) = 0% to the host until the right
		 * half's first battery notification arrives. A genuinely 0%
		 * half is about to die anyway; showing the other half's level
		 * for those final minutes is the better failure mode. */
		if (rc == 0 && periph > 0 && periph < level) {
			level = periph;
		}
	}

	if (bt_bas_get_battery_level() != level) {
		int rc = bt_bas_set_battery_level(level);

		if (rc != 0) {
			LOG_WRN("nocfree bas min: set level %u failed (%d)", level, rc);
		} else {
			LOG_DBG("nocfree bas min: host BAS=%u", level);
		}
	}

	/* Keep host fresh when a half reconnects without a new sample. */
	k_work_reschedule(&bas_min_work, K_SECONDS(30));
}

static void schedule_apply(void)
{
	/* Coalesce left + peripheral updates; beat the next left sample. */
	k_work_reschedule(&bas_min_work, K_MSEC(50));
}

static int bas_min_listener(const zmk_event_t *eh)
{
	ARG_UNUSED(eh);
	schedule_apply();
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nocfree_bas_min, bas_min_listener);
ZMK_SUBSCRIPTION(nocfree_bas_min, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(nocfree_bas_min, zmk_peripheral_battery_state_changed);

static int bas_min_init(void)
{
	schedule_apply();
	return 0;
}

/* After ZMK battery (APPLICATION default); fixed level for the linker. */
SYS_INIT(bas_min_init, APPLICATION, 99);
