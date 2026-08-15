/*
 * 1200-baud USB touch -> reboot into the Adafruit UF2 bootloader.
 *
 * WHY THIS EXISTS
 * ---------------
 * The stock firmware implements this: a CDC line-state handler watches for a
 * DTR drop while the line coding is 1200 baud, then resets into the
 * bootloader. Flashing ZMK deletes that handler, so we reimplement it --
 * otherwise:
 *
 *   - the DONGLE has no keys, so &bootloader can never be bound, and
 *   - the HALVES have no reset pinhole, so double-tap DFU (which DOES exist
 *     on this Adafruit nRF52833 bootloader) is unreachable without opening
 *     the case.
 *
 * A build that boots with a broken kscan would then be unrecoverable. This is the
 * seatbelt. Verified live against stock firmware: dfu_touch.py flipped the dongle
 * from VID_2886/PID_8029 to VID_239A/PID_002A (Adafruit bootloader) and back.
 *
 * ONE DELIBERATE DIFFERENCE FROM STOCK
 * ------------------------------------
 * Stock used magic 0x4e (serial-only DFU), which is why the touch produced a COM
 * port rather than a drive. We use 0x57 (UF2 mass storage) so recovery is
 * drag-and-drop -- the same magic ZMK's own &bootloader behaviour uses.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usb_device.h>

#include <hal/nrf_power.h>

/* Adafruit nRF52 bootloader magics, written to GPREGRET before a cold reset. */
#define DFU_MAGIC_UF2_RESET 0x57 /* mass-storage drive: drag-and-drop a .uf2 */

#define TRIGGER_BAUD CONFIG_NOCFREE_USB_RECOVERY_BAUD
#define POLL_MS CONFIG_NOCFREE_USB_RECOVERY_POLL_MS

static const struct device *const cdc_dev =
	DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

static void enter_bootloader(void)
{
	printk("nocfree: 1200-baud touch seen; entering UF2 bootloader\n");

	/*
	 * Let the log drain over USB. This runs on the system work queue, so it
	 * blocks it for 50 ms -- acceptable only because sys_reboot() below never
	 * returns and nothing else is going to matter.
	 */
	k_msleep(50);

	/*
	 * Write GPREGRET directly rather than via nrf_power_gpregret_set(): that
	 * helper's signature changed across nrfx versions (older takes (p_reg, value),
	 * newer takes (p_reg, reg_num, value)), so calling it is a coin-flip on which
	 * Zephyr tree ZMK pulls in. The register write is unambiguous, and GPREGRET is
	 * exactly what the Adafruit bootloader reads.
	 */
	NRF_POWER->GPREGRET = DFU_MAGIC_UF2_RESET;
	sys_reboot(SYS_REBOOT_COLD);
}

static void poll_line_state(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_line_state);

static void poll_line_state(struct k_work *work)
{
	static bool dtr_was_high;
	static bool seen_enumeration;
	uint32_t dtr = 0;
	uint32_t baud = 0;

	ARG_UNUSED(work);

	if (uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr) == 0) {
		/*
		 * DTR is documented as unreliable before the host has enumerated
		 * and opened the port (Zephyr issue #15759), so ignore everything
		 * until we have seen DTR genuinely asserted at least once. Without
		 * this, garbage during enumeration could look like a falling edge
		 * and drop the device into the bootloader on every boot -- which on
		 * the dongle would look exactly like a brick.
		 */
		if (!seen_enumeration) {
			if (dtr) {
				seen_enumeration = true;
				dtr_was_high = true;
			}
			goto reschedule;
		}

		/*
		 * The trigger is the FALLING edge of DTR while the host has set
		 * 1200 baud -- i.e. the port was opened and then closed. Testing
		 * the level alone would fire spuriously on any 1200-baud session.
		 */
		if (dtr_was_high && dtr == 0) {
			if (uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_BAUD_RATE,
					       &baud) == 0 &&
			    baud == TRIGGER_BAUD) {
				enter_bootloader();
				CODE_UNREACHABLE;
			}
		}
		dtr_was_high = (dtr != 0);
	}

reschedule:
	k_work_schedule(&poll_work, K_MSEC(POLL_MS));
}

static int nocfree_recovery_init(void)
{
#if !IS_ENABLED(CONFIG_ZMK_USB)
	/*
	 * Belt and braces. NOTE: the original justification for this block was
	 * wrong -- ZMK compiles src/usb.c under CONFIG_USB_DEVICE_STACK (not
	 * ZMK_USB), so it calls usb_enable() at APPLICATION 96 on the halves too.
	 * Our call therefore normally returns -EALREADY, which we tolerate. It is
	 * kept only so this shim does not depend on that remaining true: on a
	 * device with no reset button, the cost of an extra idempotent call is
	 * nothing next to the cost of USB never coming up.
	 */
	{
		int err = usb_enable(NULL);

		if (err && err != -EALREADY) {
			printk("nocfree: usb_enable failed (%d) -- USB RECOVERY IS NOT ACTIVE\n", err);
			return err;
		}
	}
#endif

	if (cdc_dev == NULL || !device_is_ready(cdc_dev)) {
		/*
		 * Loud, because losing this silently means losing the only way
		 * back into the bootloader on a device with no buttons.
		 */
		printk("nocfree: CDC console not ready -- USB RECOVERY IS NOT ACTIVE\n");
		return -ENODEV;
	}

	printk("nocfree: USB recovery armed (%d baud touch)\n", TRIGGER_BAUD);
	k_work_schedule(&poll_work, K_MSEC(POLL_MS));
	return 0;
}

/*
 * Priority 97, deliberately: ZMK calls usb_enable() at APPLICATION priority 96
 * (ZMK_USB_INIT_PRIORITY). Running at the default 90 happened to work only
 * because the CDC device object itself initialises at POST_KERNEL -- that is
 * luck, not ordering. Sit after USB is actually up.
 *
 * Polling rather than a callback because Zephyr's CDC ACM exposes no line-state
 * callback equivalent to TinyUSB's tud_cdc_line_state_cb.
 */
SYS_INIT(nocfree_recovery_init, APPLICATION, 97);
