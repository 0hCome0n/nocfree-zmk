/*
 * Clear the PCA9555 polarity-inversion registers at boot.
 *
 * The stock firmware writes POLARITY0/1 = 0xFF (inverted) into all three
 * expanders on every boot. Those registers survive anything short of expander
 * POWER loss, and Zephyr's gpio_pca95xx driver never touches them (REG_POL_INV_*
 * are defined at gpio_pca95xx.c:44-45 and used nowhere).
 *
 * So a half flashed warm -- dfu_touch, DFU, ZMK, no battery disconnect --
 * inherits that inversion, and under ZMK every idle key reads PRESSED:
 * 48 phantom holds from the first scan, the kscan absolute-time ratchet
 * saturates the system workqueue, and physical presses arrive as releases.
 * Believed to be the mechanism of the 2026-08-08 right-half failure (the
 * left escaped by having its expanders power-cycled along the way).
 *
 * Six bytes of I2C, idempotent, both halves, every boot. Runs after the
 * expanders initialise (POST_KERNEL, GPIO_PCA95XX_INIT_PRIORITY=60) and
 * before anything scans. Failure is reported but never fails the boot:
 * kscan's own error path will make a dead bus loud, and a half that boots
 * is a half that can be recovered over USB.
 *
 * READ-BACK: adopted from the community port at
 * github.com/NocFreeKB/NocFree-and-zmk, whose PCA9555 scan driver verifies
 * these registers where this only wrote them. A successful i2c_write proves
 * the byte was ACKed, not that the register holds it -- and the failure mode
 * is not subtle: an inversion left in place means every idle key reads
 * PRESSED. Two extra I2C bytes per expander turn that into one loud line at
 * boot.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#define REG_POL_INV_PORT0 0x04

static int nocfree_clear_expander_polarity(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	static const uint8_t addrs[] = {0x20, 0x22, 0x24};
	/* Register pointer + both polarity ports; the PCA9555 auto-increments
	 * the register pointer, so one 3-byte write clears both. */
	static const uint8_t clear[] = {REG_POL_INV_PORT0, 0x00, 0x00};
	int failed = 0;

	if (!device_is_ready(bus)) {
		printk("nocfree: i2c0 not ready -- expander polarity NOT cleared\n");
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(addrs); i++) {
		static const uint8_t reg = REG_POL_INV_PORT0;
		uint8_t readback[2];
		int err = i2c_write(bus, clear, sizeof(clear), addrs[i]);

		if (err != 0) {
			printk("nocfree: polarity clear failed on expander 0x%02x (%d)\n",
			       addrs[i], err);
			failed++;
			continue;
		}

		/* An ACKed write is not proof the register took it. Confirm. */
		err = i2c_write_read(bus, addrs[i], &reg, sizeof(reg), readback,
				     sizeof(readback));
		if (err != 0) {
			printk("nocfree: polarity READBACK failed on 0x%02x (%d) -- "
			       "cannot confirm; expect phantom keys if it did not take\n",
			       addrs[i], err);
			failed++;
		} else if (readback[0] != 0x00 || readback[1] != 0x00) {
			printk("nocfree: expander 0x%02x STILL INVERTING (pol=%02x%02x) -- "
			       "every idle key will read as pressed\n",
			       addrs[i], readback[0], readback[1]);
			failed++;
		}
	}

	if (failed == 0) {
		printk("nocfree: expander polarity registers cleared and verified\n");
	}

	return 0;
}

/* Between the expanders (POST_KERNEL 60) and the kscan device (POST_KERNEL,
 * KSCAN_INIT_PRIORITY=70). */
SYS_INIT(nocfree_clear_expander_polarity, POST_KERNEL, 65);
