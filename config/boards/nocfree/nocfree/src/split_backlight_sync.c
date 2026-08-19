/*
 * NOCFREE SPLIT BACKLIGHT SYNC -- central (left) only, keeper feature.
 *
 * THE PROBLEM: this ZMK version has no split backlight sync and raises no
 * backlight-state-changed event. The central owns the keymap, so &bl presses
 * (Fn+F5/F6/Tab) only ever adjust the LEFT half's pwmleds; the right half
 * would sit at its boot default forever.
 *
 * THE MECHANISM (no new GATT plumbing): the split transport already carries
 * central->peripheral behavior invocation -- zmk_split_central_invoke_behavior
 * (src/split/central.c) writes the RUN_BEHAVIOR characteristic, and the
 * peripheral runs the named behavior locally (src/split/peripheral.c). &bl
 * accepts absolute commands (BL_ON_CMD / BL_OFF_CMD / BL_SET_CMD + 0-100,
 * dt-bindings/zmk/backlight.h), so mirroring is exact and idempotent.
 *
 * THE TRIGGER: since no event exists, a slow delayed-work poll reads the
 * central's state (zmk_backlight_is_on/get_brt) and pushes on change. A
 * failed send (peripheral not linked yet) leaves the state dirty, so the
 * next tick retries -- that is also the initial-sync and reconnect path.
 * Poll cost is one BLE write per second ONLY while state keeps failing;
 * steady state costs a comparison per second.
 *
 * &bl's release handler is a no-op, so a single "pressed" invoke per change
 * is the complete interaction.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <dt-bindings/zmk/backlight.h>

#include <zmk/activity.h>
#include <zmk/backlight.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/central.h>

/* Resolve the peripheral-side behavior device name at compile time with the
 * same macro the keymap uses (zmk/keymap.h: DEVICE_DT_NAME = the node's
 * "label" prop, falling back to the node name), so this can never drift from
 * what &bl binds to. The node is literally named "bcklight" -- <=8 chars was
 * a split-payload constraint; DEVICE_DT_NAME_GET is the struct-device form
 * and does NOT compile here. */
#define BL_DEV DEVICE_DT_NAME(DT_NODELABEL(bl))

static bool have_last;
static bool last_on;
static uint8_t last_brt;

static int push_to(uint8_t source, uint32_t cmd, uint32_t arg) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = BL_DEV,
        .param1 = cmd,
        .param2 = arg,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0, /* unused by &bl */
        .timestamp = k_uptime_get(),
    };
    return zmk_split_central_invoke_behavior(source, &binding, event, true);
}

static void sync_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sync_work, sync_work_handler);

static void sync_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    bool on = zmk_backlight_is_on();
    uint8_t brt = zmk_backlight_get_brt();

    bool changed =
        !have_last || on != last_on || (on && brt != last_brt);
    if (!changed) {
        goto out;
    }

    bool ok = true;
    for (uint8_t src = 0; src < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; src++) {
        int rc;
        if (!on) {
            rc = push_to(src, BL_OFF_CMD, 0);
        } else {
            /* ON first (restores peripheral's own level), then SET pins the
             * exact brightness. ON when already on is a no-op. */
            rc = push_to(src, BL_ON_CMD, 0);
            if (rc == 0) {
                rc = push_to(src, BL_SET_CMD, brt);
            }
        }
        if (rc != 0) {
            /* NOTE (2026-08-14 review): this branch is nearly unreachable —
             * zmk_split_central_invoke_behavior enqueues to a msgq and
             * returns 0 even when the peripheral is DISCONNECTED (the drop
             * happens later, async). Reconnect resync is therefore handled
             * by the peripheral-status listener below, not by this rc. The
             * check stays for the real msgq-full / bad-args failures. */
            ok = false;
        }
    }

    if (ok) {
        printk("BLSYNC pushed: %s %u%%\n", on ? "ON" : "OFF", brt);
        have_last = true;
        last_on = on;
        last_brt = brt;
    }

out:
#if IS_ENABLED(CONFIG_NOCFREE_ACTIVITY_SYNC)
    /* Activity nudge: while THIS half (central — it sees every key from both
     * halves) is ACTIVE, periodically invoke the sentinel "nudge" on the
     * peripheral. The patched peripheral handler (zmk/app/src/split/
     * peripheral.c) resets its idle clock on any invoke and skips dispatch
     * for the sentinel, so no behavior device needs to exist. Result: the
     * right's idle-off and deep sleep track the SESSION, not its own keys —
     * no more mid-session dark right / deep-sleep drop during left-heavy
     * use. Nudges stop when the left idles, so the halves idle out together
     * and a truly abandoned board still sleeps. Push errors are ignored:
     * unlinked peripherals need no nudging.
     */
    {
        static uint8_t nudge_ticks;

        if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE &&
            ++nudge_ticks >= MAX(1, CONFIG_NOCFREE_ACTIVITY_SYNC_PERIOD_MS /
                                        CONFIG_NOCFREE_BACKLIGHT_SYNC_POLL_MS)) {
            nudge_ticks = 0;
            struct zmk_behavior_binding nudge_binding = {.behavior_dev = "nudge"};
            struct zmk_behavior_binding_event nudge_event = {
                .position = 0,
                .timestamp = k_uptime_get(),
            };
            for (uint8_t src = 0; src < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; src++) {
                (void)zmk_split_central_invoke_behavior(src, &nudge_binding, nudge_event, true);
            }
        }
    }
#endif
    k_work_schedule(&sync_work,
                    K_MSEC(CONFIG_NOCFREE_BACKLIGHT_SYNC_POLL_MS));
}

/* Reconnect resync (2026-08-14, replaces the dead rc-based retry): pushes to
 * a disconnected peripheral return 0 and vanish in the async transport, so
 * the poll happily latched state the right half never received — brightness
 * and on/off drifted across every disconnect/reconnect (deep-sleep wake, link
 * blip). Any peripheral status change marks the state dirty; the next poll
 * tick re-pushes to everyone. Cheap, and correct for multi-peripheral. */
static int sync_peripheral_status_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    have_last = false;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nocfree_bl_sync_periph, sync_peripheral_status_listener);
ZMK_SUBSCRIPTION(nocfree_bl_sync_periph, zmk_split_peripheral_status_changed);

static int split_backlight_sync_init(void) {
    k_work_schedule(&sync_work,
                    K_MSEC(CONFIG_NOCFREE_BACKLIGHT_SYNC_START_DELAY_MS));
    return 0;
}

SYS_INIT(split_backlight_sync_init, APPLICATION, 85);
