/*
 * NocFree dongle bridge — phase 3 (HOG → USB HID).
 *
 * Flash ONLY as trial (autodfu+WDT). See docs/DONGLE_SAFETY.md.
 *
 * Test: left half on Bluetooth (or 2.4G profile the dongle bonds to),
 * advertising "NocFree &". Dongle USB into PC → type on keyboard.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>

#include "bridge.h"

int main(void)
{
	int err;

	printk("bridge: NocFree dongle bridge HOG client\n");
#if IS_ENABLED(CONFIG_NOCFREE_TRIAL_AUTODFU)
	printk("bridge: PID 0x9029 — TRIAL autodfu active\n");
#elif IS_ENABLED(CONFIG_NOCFREE_BRIDGE_KEEPER)
	printk("bridge: PID 0x9029 — KEEPER (no autodfu; recovery armed)\n");
#else
	printk("bridge: PID 0x9029\n");
#endif

	/*
	 * Register HID *before* usb_enable so the composite descriptor includes
	 * the keyboard interface. Enabling first then registering can leave
	 * Windows with a silent HID (no int_in_ready → all reports dropped).
	 */
#if IS_ENABLED(CONFIG_NOCFREE_BRIDGE_USB_HID_ENABLE)
	err = bridge_usb_hid_init();
	if (err) {
		printk("bridge: USB HID init failed: %d\n", err);
	}
#endif

	err = usb_enable(NULL);
	if (err && err != -EALREADY) {
		printk("bridge: usb_enable failed: %d\n", err);
	} else {
		printk("bridge: USB enabled (CDC recovery + HID)\n");
	}

#if IS_ENABLED(CONFIG_NOCFREE_BRIDGE_BLE_ENABLE)
	err = bridge_ble_init();
	if (err) {
		printk("bridge: BLE init failed: %d\n", err);
	}
#endif

	while (1) {
		k_sleep(K_SECONDS(30));
#if IS_ENABLED(CONFIG_NOCFREE_BRIDGE_USB_HID_ENABLE)
		{
			uint32_t ok = 0, drops = 0;
			uint8_t q = 0;

			bridge_usb_hid_stats(&ok, &drops, &q);
#if IS_ENABLED(CONFIG_NOCFREE_TRIAL_AUTODFU)
			printk("bridge: alive trial ok=%u drops=%u q=%u\n", ok, drops, q);
#else
			printk("bridge: alive ok=%u drops=%u q=%u\n", ok, drops, q);
#endif
		}
#else
		printk("bridge: alive\n");
#endif
	}

	return 0;
}
