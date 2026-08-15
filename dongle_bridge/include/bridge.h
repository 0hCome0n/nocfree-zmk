#pragma once

#include <zephyr/kernel.h>

/** USB HID keyboard + consumer device. */
int bridge_usb_hid_init(void);

/** BLE central: enable stack, start scanning for "NocFree &". */
int bridge_ble_init(void);

/** Forward an 8-byte boot keyboard report to USB (mods+rsvd+6 keys). Queued. */
int bridge_usb_hid_send_keyboard(const uint8_t *report8);

/**
 * Forward a 12-byte consumer report body (6 x uint16 LE usages, ZMK FULL).
 * Report ID 2 is prepended on the USB wire.
 */
int bridge_usb_hid_send_consumer(const uint8_t *report12);

/** Send all-keys-up on keyboard + empty consumer (stuck-key guard). */
int bridge_usb_hid_release_all(void);

/** Optional stats for diagnostics (ok TX, queue overflows, current depth). */
void bridge_usb_hid_stats(uint32_t *ok, uint32_t *drops, uint8_t *queued);
