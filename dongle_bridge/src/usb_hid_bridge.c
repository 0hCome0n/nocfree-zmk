/*
 * USB HID face for the bridge (classic Zephyr USB HID class).
 * Queues keyboard + consumer reports so BLE notifies are not dropped when
 * the IN endpoint is busy (-EAGAIN).
 *
 * Wire format matches ZMK HOG on the left half:
 *   Report ID 1 — boot keyboard (8-byte body)
 *   Report ID 2 — consumer FULL (6 x uint16 usages)
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include "bridge.h"

#define KB_REPORT_SIZE 8
#define CONS_REPORT_SIZE 12 /* 6 x uint16 */
#define HID_Q_DEPTH 48
#define HID_BODY_MAX CONS_REPORT_SIZE

enum hid_rpt_kind {
	HID_RPT_KB = 1,
	HID_RPT_CONS = 2,
};

struct hid_q_item {
	uint8_t kind; /* HID_RPT_KB or HID_RPT_CONS */
	uint8_t len;
	uint8_t body[HID_BODY_MAX];
};

static const struct device *hid_dev;

static struct hid_q_item hid_q[HID_Q_DEPTH];
static uint8_t hid_q_head; /* next write */
static uint8_t hid_q_tail; /* next read */
static uint8_t hid_q_count;
static struct k_spinlock hid_q_lock;
static atomic_t hid_q_drops;
static atomic_t hid_tx_ok;

/*
 * HID report map: keyboard (ID 1) + consumer control FULL (ID 2).
 * Consumer layout matches CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_FULL on the
 * left half (6 array usages, 16-bit each).
 */
static const uint8_t hid_report_desc[] = {
	/* ---- Keyboard (Report ID 1) ---- */
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */
	0x85, 0x01, /*   Report ID (1) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0xE0, /*   Usage Minimum (224) */
	0x29, 0xE7, /*   Usage Maximum (231) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x08, /*   Report Count (8) */
	0x81, 0x02, /*   Input (Data,Var,Abs) — modifiers */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x08, /*   Report Size (8) */
	0x81, 0x01, /*   Input (Const) — reserved */
	0x95, 0x06, /*   Report Count (6) */
	0x75, 0x08, /*   Report Size (8) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x65, /*   Logical Maximum (101) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0x00, /*   Usage Minimum (0) */
	0x29, 0x65, /*   Usage Maximum (101) */
	0x81, 0x00, /*   Input (Data,Array) — key array */
	0xC0,       /* End Collection */

	/* ---- Consumer Control FULL (Report ID 2) ---- */
	0x05, 0x0C,       /* Usage Page (Consumer) */
	0x09, 0x01,       /* Usage (Consumer Control) */
	0xA1, 0x01,       /* Collection (Application) */
	0x85, 0x02,       /*   Report ID (2) */
	0x15, 0x00,       /*   Logical Minimum (0) */
	0x26, 0xFF, 0xFF, /*   Logical Maximum (65535) */
	0x19, 0x00,       /*   Usage Minimum (0) */
	0x2A, 0xFF, 0xFF, /*   Usage Maximum (65535) */
	0x75, 0x10,       /*   Report Size (16) */
	0x95, 0x06,       /*   Report Count (6) */
	0x81, 0x00,       /*   Input (Data,Array,Abs) */
	0xC0,             /* End Collection */
};

static void hid_queue_push(uint8_t kind, const uint8_t *body, uint8_t len)
{
	k_spinlock_key_t key = k_spin_lock(&hid_q_lock);

	if (hid_q_count >= HID_Q_DEPTH) {
		/* Drop oldest, keep newest (prefer latest state for HOG). */
		hid_q_tail = (hid_q_tail + 1U) % HID_Q_DEPTH;
		hid_q_count--;
		atomic_inc(&hid_q_drops);
	}
	hid_q[hid_q_head].kind = kind;
	hid_q[hid_q_head].len = len;
	memcpy(hid_q[hid_q_head].body, body, len);
	if (len < HID_BODY_MAX) {
		memset(&hid_q[hid_q_head].body[len], 0, HID_BODY_MAX - len);
	}
	hid_q_head = (hid_q_head + 1U) % HID_Q_DEPTH;
	hid_q_count++;
	k_spin_unlock(&hid_q_lock, key);
}

static int hid_queue_peek_copy(struct hid_q_item *out)
{
	k_spinlock_key_t key = k_spin_lock(&hid_q_lock);

	if (hid_q_count == 0) {
		k_spin_unlock(&hid_q_lock, key);
		return -EAGAIN;
	}
	*out = hid_q[hid_q_tail];
	k_spin_unlock(&hid_q_lock, key);
	return 0;
}

static void hid_queue_pop(void)
{
	k_spinlock_key_t key = k_spin_lock(&hid_q_lock);

	if (hid_q_count > 0) {
		hid_q_tail = (hid_q_tail + 1U) % HID_Q_DEPTH;
		hid_q_count--;
	}
	k_spin_unlock(&hid_q_lock, key);
}

/*
 * Serialize the peek→write→pop cycle (2026-08-18 review). This function runs
 * from THREE thread contexts: BT RX (notify_func → send_*), the USB stack
 * thread (int_in_ready_cb) and the reconnect queue (release_all). The queue
 * spinlock makes each queue op atomic but not the SEQUENCE: two concurrent
 * drainers could peek the same item — duplicate send — and then both pop,
 * discarding a never-sent report behind it. HID reports are absolute state,
 * so a discarded report self-corrects on the next key event — but if it was
 * the final release of a burst, the host holds that key until the user types
 * again. K_NO_WAIT (never block the BT RX thread) + the post-unlock re-check:
 * a push that loses the lock race returns immediately, and the holder's
 * re-check picks the item up, so nothing is stranded until the next event.
 */
static K_MUTEX_DEFINE(hid_drain_lock);

static void hid_try_drain(void)
{
	struct hid_q_item item;
	uint8_t buf[1 + HID_BODY_MAX];
	int err;

	if (!hid_dev) {
		return;
	}

	while (true) {
		if (k_mutex_lock(&hid_drain_lock, K_NO_WAIT) != 0) {
			/* Someone is draining; their re-check covers our push. */
			return;
		}
		while (hid_queue_peek_copy(&item) == 0) {
			buf[0] = item.kind;
			memcpy(&buf[1], item.body, item.len);
			err = hid_int_ep_write(hid_dev, buf, (uint16_t)(1U + item.len), NULL);
			if (err == -EAGAIN) {
				/* Endpoint busy; int_in_ready will call us again. */
				k_mutex_unlock(&hid_drain_lock);
				return;
			}
			if (err) {
				printk("bridge/usb: hid write err %d (id=%u)\n", err, item.kind);
				/* Drop this report so we do not stall forever. */
				hid_queue_pop();
				continue;
			}
			hid_queue_pop();
			atomic_inc(&hid_tx_ok);
		}
		k_mutex_unlock(&hid_drain_lock);
		/* Re-check: a push between our last peek and the unlock lost the
		 * lock race and returned — drain again rather than strand it. */
		if (hid_queue_peek_copy(&item) != 0) {
			return;
		}
	}
}

static void int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	hid_try_drain();
}

static const struct hid_ops ops = {
	.int_in_ready = int_in_ready_cb,
};

int bridge_usb_hid_init(void)
{
	hid_dev = device_get_binding("HID_0");
	if (!hid_dev) {
		printk("bridge/usb: HID_0 not found\n");
		return -ENODEV;
	}

	usb_hid_register_device(hid_dev, hid_report_desc, sizeof(hid_report_desc),
				&ops);
	int err = usb_hid_init(hid_dev);
	if (err) {
		printk("bridge/usb: usb_hid_init failed: %d\n", err);
		return err;
	}

	printk("bridge/usb: HID kb+consumer registered (ID1/ID2, q=%u)\n", HID_Q_DEPTH);
	return 0;
}

int bridge_usb_hid_send_keyboard(const uint8_t *report8)
{
	if (!hid_dev || !report8) {
		return -ENODEV;
	}

	hid_queue_push(HID_RPT_KB, report8, KB_REPORT_SIZE);
	hid_try_drain();
	return 0;
}

int bridge_usb_hid_send_consumer(const uint8_t *report12)
{
	if (!hid_dev || !report12) {
		return -ENODEV;
	}

	hid_queue_push(HID_RPT_CONS, report12, CONS_REPORT_SIZE);
	hid_try_drain();
	return 0;
}

int bridge_usb_hid_release_all(void)
{
	static const uint8_t kb_zeros[KB_REPORT_SIZE];
	static const uint8_t cons_zeros[CONS_REPORT_SIZE];
	int err;

	err = bridge_usb_hid_send_keyboard(kb_zeros);
	(void)bridge_usb_hid_send_consumer(cons_zeros);
	return err;
}

void bridge_usb_hid_stats(uint32_t *ok, uint32_t *drops, uint8_t *queued)
{
	if (ok) {
		*ok = (uint32_t)atomic_get(&hid_tx_ok);
	}
	if (drops) {
		*drops = (uint32_t)atomic_get(&hid_q_drops);
	}
	if (queued) {
		k_spinlock_key_t key = k_spin_lock(&hid_q_lock);

		*queued = hid_q_count;
		k_spin_unlock(&hid_q_lock, key);
	}
}
