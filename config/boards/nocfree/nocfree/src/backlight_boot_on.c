/*
 * NOCFREE RIGHT-HALF BACKLIGHT BOOT-ON -- peripheral (right) only, keeper feature.
 *
 * CORRECTED RATIONALE (2026-08-15). The original header blamed a persisted
 * OFF restored by settings_load(); that was FALSE — the zmk-local backlight.c
 * patch compiles settings persistence out of peripherals entirely, so there
 * is nothing to restore. The 08-14 review caught the false premise and this
 * module was deleted as "redundant with CONFIG_ZMK_BACKLIGHT_ON_START=y" —
 * and the very next right-half trial BOOTED DARK. Empirical truth: ON_START
 * sets the initial state, but the init-time apply does not reach this
 * board's LED (init ordering vs the backlight path). The delayed re-assert
 * below is what actually lights the half at boot, and always has been.
 *
 * Mechanism: delayed work (~1 s, past all init) calls zmk_backlight_on(),
 * which keeps the current brightness level — no jump relative to the left.
 *
 * TRADEOFF: if the backlight is DELIBERATELY off, the right still boots on
 * for ~1 s until the live sync pushes off and corrects it — a brief wrong-on
 * that self-corrects. Right-half local: no BLE callbacks, no central code,
 * no dongle path — deliberately unlike an earlier reverted connect-callback
 * attempt that fired for the dongle link too and stalled reconnection.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <zmk/backlight.h>

static void boot_on_work_handler(struct k_work *work) {
	ARG_UNUSED(work);
	/* Runs after settings_load(): re-light the mirrored slave state. */
	(void)zmk_backlight_on();
}

static K_WORK_DELAYABLE_DEFINE(boot_on_work, boot_on_work_handler);

static int nocfree_backlight_boot_on_init(void) {
	k_work_schedule(&boot_on_work, K_MSEC(CONFIG_NOCFREE_BACKLIGHT_BOOT_ON_DELAY_MS));
	return 0;
}

SYS_INIT(nocfree_backlight_boot_on_init, APPLICATION, 85);
