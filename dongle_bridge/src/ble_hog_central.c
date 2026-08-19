/*
 * BLE central HOG client — phase 3.
 *
 * Scan for "NocFree &" (left half; NAME + HIDS UUID both required — see
 * ad_is_nocfree_keyboard), connect + bond, discover HIDS input reports,
 * subscribe (verified — CCC write results, not queue-time optimism), forward
 * keyboard/consumer body to USB HID.
 *
 * Recovery machinery runs on its own work queue (never the system workqueue:
 * hard resets block, and the BT host shuts down THROUGH the sysworkq).
 * Keeper-deployed since 2026-08-10; new code still trials first
 * (DONGLE_SAFETY.md). Does not touch stock dongle topology / ZMK split.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include "bridge.h"

#define TARGET_NAME "NocFree &"
#define TARGET_NAME_LEN (sizeof(TARGET_NAME) - 1)

#define HIDS_INPUT 0x01
#define REPORT_ID_KEYBOARD 0x01
#define REPORT_ID_CONSUMER 0x02
#define KB_BODY_LEN 8
#define CONSUMER_BODY_LEN 12 /* 6 x uint16 */

enum disc_state {
	DISC_IDLE = 0,
	DISC_HIDS_SVC,
	DISC_HIDS_CHARS,
	DISC_DESC,
	DISC_DONE,
};

static struct bt_conn *default_conn;
static uint16_t hids_start, hids_end;
static enum disc_state disc_state;

/* Up to 4 input report chars (keyboard, consumer, mouse, spare). */
#define MAX_INPUT_REPORTS 4
struct input_report {
	uint16_t value_handle;
	uint16_t ccc_handle;
	uint8_t report_id;
	uint8_t report_type;
	bool subscribed;
	struct bt_gatt_subscribe_params sub;
};
static struct input_report inputs[MAX_INPUT_REPORTS];
static int input_count;
static int desc_index; /* which input we're describing */

static struct bt_uuid_16 uuid_hids = BT_UUID_INIT_16(BT_UUID_HIDS_VAL);
static struct bt_uuid_16 uuid_report = BT_UUID_INIT_16(BT_UUID_HIDS_REPORT_VAL);

static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_read_params read_params;

static void scan_start(void);
static void start_discovery(struct bt_conn *conn);
static void discover_next_desc(struct bt_conn *conn);
static void drop_bond_and_retry(struct bt_conn *conn, const char *why);
static void reconnect_schedule(void);
static void link_reset_soft(const char *why);
static void link_reset_hard(const char *why);
static bool conn_is_live(struct bt_conn *conn);
static uint8_t reconnect_attempts;
static uint8_t hog_stall_ticks;
static bool bt_stack_ready;
static bool scanning;

static void try_release_keys(void)
{
	(void)bridge_usb_hid_release_all();
}

/*
 * Host-link connection parameters. The dongle is the CENTRAL: it owns these,
 * and le_param_req() below clamps any peripheral counter-request to them.
 *
 * STRICT 15 ms interval, not 7.5: the left's single radio also runs the
 * 7.5 ms split link to the right half. Two 7.5 ms-class schedules collide,
 * and every collision delays a right-half key past a left-half one —
 * observed as intermittent key reordering, worse in 2.4G than BT (Windows
 * negotiates a slacker host link than the old 7.5-15 ms request here did).
 * 15 ms guarantees a free 7.5 ms slot for the split link between every
 * host-link event. Cost: +7.5 ms uniform report latency — cannot reorder.
 *
 * Latency 2 lets the idle left skip 2 events (battery); worst-case resume
 * lag 3 x 15 = 45 ms, the balance accepted in the halves' tuning.
 */
static const struct bt_le_conn_param host_link_param = {
	.interval_min = 12, /* 12 * 1.25 ms = 15 ms */
	.interval_max = 12,
	.latency = 2,
	.timeout = 400, /* 4 s supervision */
};

static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params, const void *data,
			   uint16_t length)
{
	ARG_UNUSED(conn);

	if (!data) {
		printk("bridge/ble: unsubscribed handle 0x%04x\n", params->value_handle);
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	/* Match which report by value handle */
	uint8_t report_id = 0;
	for (int i = 0; i < input_count; i++) {
		if (inputs[i].value_handle == params->value_handle) {
			report_id = inputs[i].report_id;
			break;
		}
	}

	/* Unknown id (report refs never read): only an EXACT keyboard-size body
	 * may take the keyboard fallback — `>= KB_BODY_LEN` also matched a
	 * 12-byte consumer report and typed its first 8 bytes as keys. */
	if (report_id == REPORT_ID_KEYBOARD ||
	    (report_id == 0 && (length == KB_BODY_LEN || length == KB_BODY_LEN + 1))) {
		if (length >= KB_BODY_LEN) {
			const uint8_t *body = data;
			/* Some stacks prefix the Report ID in the notify payload */
			if (length >= KB_BODY_LEN + 1 && body[0] == REPORT_ID_KEYBOARD) {
				body = &((const uint8_t *)data)[1];
			}
			/* Hot path: no printk (CDC logging dropped keys under load). */
			(void)bridge_usb_hid_send_keyboard(body);
		}
	} else if (report_id == REPORT_ID_CONSUMER) {
		if (length >= CONSUMER_BODY_LEN) {
			const uint8_t *body = data;

			if (length >= CONSUMER_BODY_LEN + 1 && body[0] == REPORT_ID_CONSUMER) {
				body = &((const uint8_t *)data)[1];
			}
			(void)bridge_usb_hid_send_consumer(body);
		}
	}

	return BT_GATT_ITER_CONTINUE;
}

static int sub_index;

static void subscribe_next(struct bt_conn *conn);

static void subscribe_write_done(struct bt_conn *conn, uint8_t err,
				 struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(params);

	if (err) {
		/* VERIFIED subscribes (2026-08-14 review): the CCC write failed,
		 * so no notifications will ever arrive on this report — do NOT
		 * mark it subscribed (the old code did, then declared HOG ready
		 * with dead keys and a happy watchdog). Leave subscribed=false;
		 * the end-of-pass check in subscribe_next resets the link. */
		printk("bridge/ble: CCC write err %u (idx %d) — NOT subscribed\n", err, sub_index);
		if (sub_index < input_count) {
			inputs[sub_index].subscribed = false;
		}
	} else {
		printk("bridge/ble: CCC write ok (idx %d)\n", sub_index);
		if (sub_index < input_count) {
			inputs[sub_index].subscribed = true;
		}
	}
	sub_index++;
	subscribe_next(conn);
}

static void subscribe_next(struct bt_conn *conn)
{
	while (sub_index < input_count) {
		if (!inputs[sub_index].ccc_handle) {
			/* ZMK HOG: value handle then CCC (+1) then Report Ref (+2) */
			inputs[sub_index].ccc_handle = inputs[sub_index].value_handle + 1;
			printk("bridge/ble: CCC fallback handle 0x%04x for value 0x%04x\n",
			       inputs[sub_index].ccc_handle, inputs[sub_index].value_handle);
		}
		if (inputs[sub_index].report_type != HIDS_INPUT ||
		    inputs[sub_index].subscribed) {
			sub_index++;
			continue;
		}

		/* Subscribe keyboard (1) + consumer (2); skip mouse/spare. */
		if (inputs[sub_index].report_id != REPORT_ID_KEYBOARD &&
		    inputs[sub_index].report_id != REPORT_ID_CONSUMER &&
		    inputs[sub_index].report_id != 0) {
			printk("bridge/ble: skip subscribe id=%u\n",
			       inputs[sub_index].report_id);
			sub_index++;
			continue;
		}

		memset(&inputs[sub_index].sub, 0, sizeof(inputs[sub_index].sub));
		inputs[sub_index].sub.notify = notify_func;
		inputs[sub_index].sub.subscribe = subscribe_write_done;
		inputs[sub_index].sub.value = BT_GATT_CCC_NOTIFY;
		inputs[sub_index].sub.ccc_handle = inputs[sub_index].ccc_handle;
		inputs[sub_index].sub.value_handle = inputs[sub_index].value_handle;

		printk("bridge/ble: subscribe id=%u value=0x%04x ccc=0x%04x\n",
		       inputs[sub_index].report_id, inputs[sub_index].value_handle,
		       inputs[sub_index].ccc_handle);

		int err = bt_gatt_subscribe(conn, &inputs[sub_index].sub);

		if (err == -EALREADY) {
			/* Params already registered from this same session — the CCC
			 * write went through before; genuinely subscribed. */
			inputs[sub_index].subscribed = true;
			sub_index++;
			continue;
		}
		if (err) {
			printk("bridge/ble: subscribe err %d — NOT subscribed\n", err);
			sub_index++;
			continue;
		}
		/* Queued only — subscribed is set by subscribe_write_done on the
		 * ACTUAL CCC write result, not here (2026-08-14 review: optimistic
		 * marking + advance-on-error reported HOG ready with dead keys). */
		return;
	}

	/* End of pass: HOG is ready only if every required input (keyboard +
	 * consumer) truly subscribed. Anything missing = this session is
	 * broken (e.g. security never reached L2, so every encrypted CCC
	 * write bounced) — reset the link for a fresh cycle instead of
	 * declaring victory with dead keys. */
	for (int i = 0; i < input_count; i++) {
		if (inputs[i].report_type != HIDS_INPUT || inputs[i].subscribed) {
			continue;
		}
		if (inputs[i].report_id == REPORT_ID_KEYBOARD ||
		    inputs[i].report_id == REPORT_ID_CONSUMER || inputs[i].report_id == 0) {
			printk("bridge/ble: subscribe incomplete (id=%u) — reset link\n",
			       inputs[i].report_id);
			link_reset_soft("subscribe incomplete");
			scan_start();
			return;
		}
	}

	disc_state = DISC_DONE;
	printk("bridge/ble: HOG ready - type on left (2.4G / profile 0)\n");
}

static void subscribe_all(struct bt_conn *conn)
{
	sub_index = 0;
	for (int i = 0; i < input_count; i++) {
		printk("bridge/ble: input[%d] id=%u type=%u value=0x%04x ccc=0x%04x\n", i,
		       inputs[i].report_id, inputs[i].report_type, inputs[i].value_handle,
		       inputs[i].ccc_handle);
	}
	subscribe_next(conn);
}

static uint8_t read_report_ref_cb(struct bt_conn *conn, uint8_t err,
				  struct bt_gatt_read_params *params, const void *data,
				  uint16_t length)
{
	ARG_UNUSED(params);

	if (err || !data || length < 2) {
		printk("bridge/ble: report ref read err=%u len=%u\n", err, length);
	} else if (desc_index < input_count) {
		const uint8_t *p = data;

		inputs[desc_index].report_id = p[0];
		inputs[desc_index].report_type = p[1];
		printk("bridge/ble: report handle 0x%04x -> id=%u type=%u\n",
		       inputs[desc_index].value_handle, p[0], p[1]);
	}

	desc_index++;
	discover_next_desc(conn);
	return BT_GATT_ITER_STOP;
}

static void read_report_ref(struct bt_conn *conn, uint16_t handle)
{
	memset(&read_params, 0, sizeof(read_params));
	read_params.func = read_report_ref_cb;
	read_params.handle_count = 1;
	read_params.single.handle = handle;
	read_params.single.offset = 0;

	int err = bt_gatt_read(conn, &read_params);

	if (err) {
		printk("bridge/ble: gatt read ref err %d\n", err);
		desc_index++;
		if (desc_index < input_count) {
			discover_next_desc(conn);
		} else {
			subscribe_all(conn);
		}
	}
}

/* Temporary: store report-ref handle per input while discovering descriptors */
static uint16_t pending_ref_handles[MAX_INPUT_REPORTS];

static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	if (!attr) {
		/* End of this discover phase */
		if (disc_state == DISC_HIDS_SVC) {
			printk("bridge/ble: HIDS service not found\n");
			return BT_GATT_ITER_STOP;
		}
		if (disc_state == DISC_HIDS_CHARS) {
			printk("bridge/ble: found %d report char(s)\n", input_count);
			if (input_count == 0) {
				return BT_GATT_ITER_STOP;
			}
			/* Discover descriptors for all report chars in one range */
			disc_state = DISC_DESC;
			memset(&discover_params, 0, sizeof(discover_params));
			discover_params.uuid = NULL;
			discover_params.func = discover_func;
			discover_params.start_handle = hids_start;
			discover_params.end_handle = hids_end;
			discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
			bt_gatt_discover(conn, &discover_params);
			return BT_GATT_ITER_STOP;
		}
		if (disc_state == DISC_DESC) {
			/* Read report refs for each input, then subscribe */
			desc_index = 0;
			bool any_ref = false;

			for (int i = 0; i < input_count; i++) {
				if (pending_ref_handles[i]) {
					any_ref = true;
					break;
				}
			}
			if (any_ref) {
				/* Find first with ref */
				while (desc_index < input_count &&
				       !pending_ref_handles[desc_index]) {
					/* default keyboard if 8-byte later */
					if (inputs[desc_index].report_id == 0) {
						inputs[desc_index].report_id = REPORT_ID_KEYBOARD;
						inputs[desc_index].report_type = HIDS_INPUT;
					}
					desc_index++;
				}
				if (desc_index < input_count) {
					read_report_ref(conn, pending_ref_handles[desc_index]);
				} else {
					subscribe_all(conn);
				}
			} else {
				/* No report ref — assume first is keyboard */
				if (input_count > 0) {
					inputs[0].report_id = REPORT_ID_KEYBOARD;
					inputs[0].report_type = HIDS_INPUT;
				}
				subscribe_all(conn);
			}
			return BT_GATT_ITER_STOP;
		}
		return BT_GATT_ITER_STOP;
	}

	if (disc_state == DISC_HIDS_SVC) {
		if (bt_uuid_cmp(params->uuid, BT_UUID_HIDS) == 0) {
			hids_start = attr->handle + 1;
			hids_end = ((struct bt_gatt_service_val *)attr->user_data)->end_handle;
			printk("bridge/ble: HIDS 0x%04x-0x%04x\n", hids_start, hids_end);

			disc_state = DISC_HIDS_CHARS;
			memset(&discover_params, 0, sizeof(discover_params));
			discover_params.uuid = &uuid_report.uuid;
			discover_params.func = discover_func;
			discover_params.start_handle = hids_start;
			discover_params.end_handle = hids_end;
			discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
			bt_gatt_discover(conn, &discover_params);
			return BT_GATT_ITER_STOP;
		}
	} else if (disc_state == DISC_HIDS_CHARS) {
		struct bt_gatt_chrc *chrc = attr->user_data;

		if (input_count < MAX_INPUT_REPORTS &&
		    (chrc->properties & BT_GATT_CHRC_NOTIFY)) {
			inputs[input_count].value_handle = chrc->value_handle;
			inputs[input_count].ccc_handle = 0;
			inputs[input_count].report_id = 0;
			inputs[input_count].report_type = HIDS_INPUT;
			inputs[input_count].subscribed = false;
			printk("bridge/ble: report char value=0x%04x props=0x%02x\n",
			       chrc->value_handle, chrc->properties);
			input_count++;
		}
		return BT_GATT_ITER_CONTINUE;
	} else if (disc_state == DISC_DESC) {
		/* Map descriptors to nearest preceding report value handle */
		uint16_t h = attr->handle;

		for (int i = input_count - 1; i >= 0; i--) {
			if (h > inputs[i].value_handle) {
				struct bt_uuid_16 *u16 = (void *)params->uuid;

				/* uuid may be null when discovering all descriptors */
				if (attr->uuid) {
					if (bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC) == 0) {
						inputs[i].ccc_handle = h;
					} else if (bt_uuid_cmp(attr->uuid,
							       BT_UUID_HIDS_REPORT_REF) == 0) {
						pending_ref_handles[i] = h;
					}
				}
				ARG_UNUSED(u16);
				break;
			}
		}
		return BT_GATT_ITER_CONTINUE;
	}

	return BT_GATT_ITER_STOP;
}

static void discover_next_desc(struct bt_conn *conn)
{
	while (desc_index < input_count && !pending_ref_handles[desc_index]) {
		if (inputs[desc_index].report_id == 0) {
			inputs[desc_index].report_id = REPORT_ID_KEYBOARD;
			inputs[desc_index].report_type = HIDS_INPUT;
		}
		desc_index++;
	}
	if (desc_index < input_count) {
		read_report_ref(conn, pending_ref_handles[desc_index]);
	} else {
		subscribe_all(conn);
	}
}

static void start_discovery(struct bt_conn *conn)
{
	input_count = 0;
	desc_index = 0;
	memset(inputs, 0, sizeof(inputs));
	memset(pending_ref_handles, 0, sizeof(pending_ref_handles));
	disc_state = DISC_HIDS_SVC;

	memset(&discover_params, 0, sizeof(discover_params));
	discover_params.uuid = &uuid_hids.uuid;
	discover_params.func = discover_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_PRIMARY;

	int err = bt_gatt_discover(conn, &discover_params);

	if (err) {
		printk("bridge/ble: discover start err %d\n", err);
	} else {
		printk("bridge/ble: discovering HIDS...\n");
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("bridge/ble: connect failed %s (err %u)\n", addr, err);
		/* Same discipline as disconnected(): only OUR conn's failure may
		 * clear state — unrefing default_conn on a foreign conn's error
		 * would orphan the live session (the batch-B stale-conn class). */
		if (conn == default_conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;
		}
		scan_start();
		return;
	}

	printk("bridge/ble: connected %s\n", addr);

	if (conn != default_conn) {
		return;
	}

	/* Re-assert the pinned host-link params (create already used them;
	 * -EALREADY is the expected happy path). */
	{
		int perr = bt_conn_le_param_update(conn, &host_link_param);

		if (perr && perr != -EALREADY) {
			printk("bridge/ble: conn param update err %d\n", perr);
		} else {
			printk("bridge/ble: host link pinned 15 ms / latency 2\n");
		}
	}

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		/* 2026-08-14 review: "discover anyway" guaranteed a dead session —
		 * ZMK's HOG CCCs are WRITE_ENCRYPT, so every subscribe write
		 * bounces on an unencrypted link (and the old optimistic marking
		 * then reported HOG ready with dead keys). Reset for a fresh
		 * cycle instead; security setup failing here is link-level, not
		 * something discovery can route around. */
		printk("bridge/ble: set_security err %d — reset for fresh cycle\n", err);
		drop_bond_and_retry(conn, "set_security failed");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	const bt_addr_le_t *dst = bt_conn_get_dst(conn);

	bt_addr_le_to_str(dst, addr, sizeof(addr));
	printk("bridge/ble: disconnected %s reason 0x%02x\n", addr, reason);

	/* 2026-08-14 review: this callback used to run its FULL cleanup for ANY
	 * connection — including a stale one whose deferred disconnect landed
	 * after device_found had already created a replacement. That orphaned
	 * the live conn (unref'd our pointer to it), wiped the bond the live
	 * session was using, and reset discovery state under it: connected-but-
	 * dead until the ~45 s hard reset. A foreign conn's death is none of
	 * our business — log and return. */
	if (default_conn != NULL && default_conn != conn) {
		printk("bridge/ble: (stale conn — live session untouched)\n");
		return;
	}

	try_release_keys();

	/*
	 * Drop ALL bonds on disconnect (NULL = every peer, which covers dst):
	 * left opens a fresh dongle-slot pair every time it enters 2.4G
	 * (mode_switch clears profile 0 at settings-commit). A stale bond here
	 * then fails security (KEY_REJECTED) and the bridge looks "totally
	 * dead".
	 */
	(void)bt_unpair(BT_ID_DEFAULT, NULL);

	if (default_conn == conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	for (int i = 0; i < MAX_INPUT_REPORTS; i++) {
		inputs[i].subscribed = false;
		memset(&inputs[i].sub, 0, sizeof(inputs[i].sub));
	}
	input_count = 0;
	disc_state = DISC_IDLE;
	hog_stall_ticks = 0;
	scanning = false;

	/* Let the left re-advertise, then scan — do not thrash immediately. */
	reconnect_schedule();
}

static void drop_bond_and_retry(struct bt_conn *conn, const char *why)
{
	char addr[BT_ADDR_LE_STR_LEN];
	const bt_addr_le_t *dst = bt_conn_get_dst(conn);

	bt_addr_le_to_str(dst, addr, sizeof(addr));
	printk("bridge/ble: %s — unpair %s and disconnect\n", why, addr);
	(void)bt_unpair(BT_ID_DEFAULT, dst);
	bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("bridge/ble: security failed %s level %u err %d\n", addr, level, err);
		/* err 9 = KEY_REJECTED (stale bond / overwrite blocked) */
		drop_bond_and_retry(conn, "security failed");
		return;
	}

	printk("bridge/ble: security %s level %u\n", addr, level);

	if (conn == default_conn && level >= BT_SECURITY_L2 && disc_state == DISC_IDLE) {
		start_discovery(conn);
	}
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	/*
	 * The peripheral (left half) auto-requests ITS preferred params after
	 * connect. Without this callback Zephyr accepts them unmodified and the
	 * peripheral's values silently replace the pin — the central must
	 * actually enforce what it owns. Answer every request with the pin.
	 */
	printk("bridge/ble: param req int %u-%u lat %u — answering with pin\n",
	       param->interval_min, param->interval_max, param->latency);
	param->interval_min = host_link_param.interval_min;
	param->interval_max = host_link_param.interval_max;
	param->latency = host_link_param.latency;
	param->timeout = host_link_param.timeout;
	return true;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	/* The GRANTED values, from the controller — ground truth, not a request. */
	printk("bridge/ble: conn params live: interval %u.%02u ms latency %u timeout %u ms\n",
	       (interval * 125U) / 100U, (interval * 125U) % 100U, latency, timeout * 10U);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
};

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("bridge/ble: pairing cancelled %s\n", addr);
}

/* Just Works: auto-accept when the stack asks the app to confirm. */
static void pairing_confirm(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("bridge/ble: pairing confirm (Just Works) %s\n", addr);
	bt_conn_auth_pairing_confirm(conn);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("bridge/ble: pairing complete %s bonded=%d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("bridge/ble: pairing failed %s reason %d\n", addr, reason);
	drop_bond_and_retry(conn, "pairing failed");
}

static struct bt_conn_auth_cb auth_cb = {
	.pairing_confirm = pairing_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static bool ad_is_nocfree_keyboard(struct net_buf_simple *ad)
{
	char name[32];
	size_t i = 0;
	bool name_ok = false;
	bool hids_uuid = false;

	name[0] = '\0';
	while (i < ad->len) {
		uint8_t len = ad->data[i++];
		uint8_t ad_type;

		if (len == 0 || (i + len) > ad->len) {
			break;
		}
		ad_type = ad->data[i++];
		len--;
		if (ad_type == BT_DATA_NAME_COMPLETE || ad_type == BT_DATA_NAME_SHORTENED) {
			size_t copy = MIN(len, sizeof(name) - 1);

			memcpy(name, &ad->data[i], copy);
			name[copy] = '\0';
			if (strncmp(name, TARGET_NAME, TARGET_NAME_LEN) == 0 ||
			    strncmp(name, "NocFree", 7) == 0) {
				name_ok = true;
			}
		} else if (ad_type == BT_DATA_UUID16_ALL || ad_type == BT_DATA_UUID16_SOME) {
			for (uint8_t j = 0; j + 1 < len; j += 2) {
				uint16_t u = sys_get_le16(&ad->data[i + j]);

				if (u == BT_UUID_HIDS_VAL) {
					hids_uuid = true;
				}
			}
		}
		i += len;
	}

	/* NAME REQUIRED (2026-08-14 review): `name_ok || hids_uuid` meant any
	 * BLE device advertising HID service 0x1812 — anyone's keyboard or
	 * mouse in range — got connected, Just-Works-bonded, and its keystrokes
	 * forwarded to the host. The left's ZMK HOG adv always carries the
	 * name, so the "directed adv may omit name" rationale never applied.
	 *
	 * HIDS UUID ALSO REQUIRED (2026-08-18 review): the left's adv carries
	 * name AND the 0x1812 UUID16 in the SAME payload (zmk ble.c zmk_ble_ad
	 * + BT_LE_ADV_OPT_FORCE_NAME_IN_AD), so requiring both is free and
	 * excludes name-only spoofers plus any HIDS-less device that happens
	 * to be called NocFree-something. (The right half is not a risk either
	 * way: its split-peripheral adv carries no name field at all.) */
	return name_ok && hids_uuid;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	if (default_conn && conn_is_live(default_conn)) {
		return;
	}
	if (default_conn && !conn_is_live(default_conn)) {
		link_reset_soft("stale default_conn in scan");
	}

	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
	    type != BT_GAP_ADV_TYPE_SCAN_RSP && type != BT_GAP_ADV_TYPE_EXT_ADV) {
		return;
	}

	if (!ad_is_nocfree_keyboard(ad)) {
		return;
	}

	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("bridge/ble: found %s rssi=%d — connecting\n", addr_str, rssi);

	(void)bt_le_scan_stop();
	scanning = false;

	struct bt_conn *conn = NULL;
	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, &host_link_param, &conn);

	if (err) {
		printk("bridge/ble: create conn err %d — rescan\n", err);
		/* -ENOMEM / -EAGAIN: often a half-dead conn still held by the stack */
		if (err == -ENOMEM || err == -EAGAIN || err == -EALREADY) {
			link_reset_soft("create failed");
		}
		reconnect_schedule();
		return;
	}

	default_conn = conn;
	reconnect_attempts = 0;
	hog_stall_ticks = 0;
}

static void scan_start(void)
{
	struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err = bt_le_scan_start(&param, device_found);

	if (err && err != -EALREADY) {
		printk("bridge/ble: scan start failed: %d\n", err);
		reconnect_schedule();
	} else {
		printk("bridge/ble: scanning for \"%s\" / HIDS\n", TARGET_NAME);
		scanning = true;
	}
}

/*
 * Recovery without USB replug — but do NOT thrash every few seconds.
 * Soft-resetting on every tick was wedging the controller (stop scan / unpair
 * mid-create). Policy:
 *  - While CONNECTING/CONNECTED and HOG not ready: wait, then one soft reset
 *  - While idle: only ensure scan is running (no unpair every tick)
 *  - Soft reset every ~15s if still idle (clear zombies)
 *  - Hard radio reset every ~45s if still idle (replug equivalent)
 */
#define RECONNECT_PERIOD_MS 5000
#define SOFT_RESET_EVERY 3   /* 3 * 5s = 15s idle */
#define HARD_RESET_EVERY 9   /* 9 * 5s = 45s idle (first one; then backs off) */
#define HOG_STALL_TICKS 4    /* ~20s connected without HOG */
#define STACK_DOWN_RETRY_TICKS 6 /* ~30s of !bt_stack_ready -> re-attempt enable */
#define HARD_BACKOFF_MAX_SHIFT 4 /* 45s,90,180,360,720s cap between hard resets */

/*
 * DEDICATED work queue (2026-08-14 review): the reconnect machinery used to
 * run on the SYSTEM workqueue, but link_reset_hard blocks for 300 ms and
 * bt_disable aborts the BT RX thread — while the BT host's own deferred conn
 * cleanup and bt_enable init are sysworkq work items. Blocking the queue the
 * stack needs to shut down cleanly is a credible root of the replug-only
 * scanner wedge. All reconnect/reset work now runs here, where it may block
 * without stalling BT or USB HID.
 */
static K_THREAD_STACK_DEFINE(reconnect_wq_stack, 2048);
static struct k_work_q reconnect_wq;

static void reconnect_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(reconnect_work, reconnect_work_handler);

static uint8_t stack_down_ticks;
static uint8_t hard_reset_count;
static uint16_t ticks_since_hard;

static void reconnect_schedule(void)
{
	k_work_reschedule_for_queue(&reconnect_wq, &reconnect_work,
				    K_MSEC(RECONNECT_PERIOD_MS));
}

static bool conn_is_live(struct bt_conn *conn)
{
	struct bt_conn_info info;

	if (!conn) {
		return false;
	}
	if (bt_conn_get_info(conn, &info)) {
		return false;
	}
	return info.state == BT_CONN_STATE_CONNECTED ||
	       info.state == BT_CONN_STATE_CONNECTING;
}

static void link_reset_soft(const char *why)
{
	printk("bridge/ble: soft reset (%s)\n", why);
	try_release_keys();
	(void)bt_le_scan_stop();
	scanning = false;

	if (default_conn) {
		if (conn_is_live(default_conn)) {
			(void)bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	(void)bt_unpair(BT_ID_DEFAULT, NULL);

	for (int i = 0; i < MAX_INPUT_REPORTS; i++) {
		inputs[i].subscribed = false;
		memset(&inputs[i].sub, 0, sizeof(inputs[i].sub));
	}
	input_count = 0;
	disc_state = DISC_IDLE;
	hog_stall_ticks = 0;
}

static void bt_ready(int err);

static void link_reset_hard(const char *why)
{
	int err;

	printk("bridge/ble: HARD reset — radio off/on (%s)\n", why);
	link_reset_soft(why);
	bt_stack_ready = false;
	(void)bt_disable();
	k_msleep(300);
	err = bt_enable(bt_ready);
	if (err) {
		printk("bridge/ble: bt_enable after hard reset err %d\n", err);
		reconnect_schedule();
	}
}

static void reconnect_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!bt_stack_ready) {
		/* PURGATORY ESCAPE (2026-08-14 review): this branch used to
		 * reschedule SILENTLY forever — if bt_enable's ready callback
		 * never fired after a hard reset, the loop ticked eternally
		 * with heartbeats alive and no scanning (the classic wedge
		 * signature: frozen `alive ok=N`, no scan lines, replug-only
		 * recovery). Now: after ~30 s stuck, re-attempt bt_enable and
		 * handle "already enabled but flag false" explicitly. */
		if (++stack_down_ticks >= STACK_DOWN_RETRY_TICKS) {
			int err;

			stack_down_ticks = 0;
			printk("bridge/ble: stack down 30s — re-attempting bt_enable\n");
			err = bt_enable(bt_ready);
			if (err == -EALREADY) {
				printk("bridge/ble: stack was up, flag stale — resuming\n");
				bt_stack_ready = true;
				scanning = false;
				scan_start();
			} else if (err) {
				printk("bridge/ble: bt_enable retry err %d\n", err);
			}
			/* err==0: bt_ready callback will restart scanning. */
		}
		reconnect_schedule();
		return;
	}
	stack_down_ticks = 0;

	/* Drop pointer to dead conns without full soft reset thrash. */
	if (default_conn && !conn_is_live(default_conn)) {
		printk("bridge/ble: drop zombie conn ptr\n");
		bt_conn_unref(default_conn);
		default_conn = NULL;
		disc_state = DISC_IDLE;
		hog_stall_ticks = 0;
	}

	if (default_conn && conn_is_live(default_conn)) {
		if (disc_state == DISC_DONE) {
			hog_stall_ticks = 0;
			reconnect_attempts = 0;
			hard_reset_count = 0;
			ticks_since_hard = 0;
		} else if (++hog_stall_ticks >= HOG_STALL_TICKS) {
			printk("bridge/ble: HOG stall — soft reset once\n");
			hog_stall_ticks = 0;
			link_reset_soft("HOG stall");
			scan_start();
		}
		reconnect_schedule();
		return;
	}

	/* Idle (not connected): keep scan alive; escalate only if stuck long.
	 * BACKOFF (2026-08-14 review): the flat 45 s hard-reset cadence ran
	 * ~600 bt_disable/enable cycles against a sleeping left every night —
	 * each one a fresh roll of the wedge dice. Escalation now doubles the
	 * hard-reset spacing each time (45 s..12 min cap) and drops the soft
	 * resets once a hard reset has proven useless; a healthy reconnect
	 * resets the ladder. */
	hog_stall_ticks = 0;
	reconnect_attempts++;
	ticks_since_hard++;

	if (ticks_since_hard >=
	    (uint16_t)(HARD_RESET_EVERY << MIN(hard_reset_count, HARD_BACKOFF_MAX_SHIFT))) {
		ticks_since_hard = 0;
		if (hard_reset_count < UINT8_MAX) {
			hard_reset_count++;
		}
		link_reset_hard("idle too long");
		return; /* bt_ready restarts scan */
	}

	if (hard_reset_count == 0 && (reconnect_attempts % SOFT_RESET_EVERY) == 0) {
		printk("bridge/ble: idle soft reset (#%u)\n", reconnect_attempts);
		link_reset_soft("idle soft");
		scan_start();
		reconnect_schedule();
		return;
	}

	/* Normal tick: only restart scan if it is not already running. */
	if (!scanning) {
		printk("bridge/ble: scan not running — start (#%u)\n", reconnect_attempts);
		scan_start();
	}
	reconnect_schedule();
}

static void bt_ready(int err)
{
	if (err) {
		printk("bridge/ble: bt_enable failed: %d\n", err);
		bt_stack_ready = false;
		reconnect_schedule();
		return;
	}

	printk("bridge/ble: stack ready\n");
	bt_stack_ready = true;
	stack_down_ticks = 0;
	scanning = false;

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	/* Fresh bonds after radio start (mode-switch reboot on left invalidates). */
	(void)bt_unpair(BT_ID_DEFAULT, NULL);
	printk("bridge/ble: bonds cleared (fresh pair each radio start)\n");

	reconnect_attempts = 0;
	hog_stall_ticks = 0;
	scan_start();
	reconnect_schedule();
}

int bridge_ble_init(void)
{
	/* Reconnect/reset machinery gets its own queue BEFORE anything can
	 * schedule onto it — see the comment at reconnect_wq. Priority below
	 * BT RX/TX threads; blocking here must never starve the stack. */
	k_work_queue_start(&reconnect_wq, reconnect_wq_stack,
			   K_THREAD_STACK_SIZEOF(reconnect_wq_stack), K_PRIO_PREEMPT(12), NULL);
	k_thread_name_set(&reconnect_wq.thread, "bridge_reconnect");

	bt_conn_auth_cb_register(&auth_cb);
	bt_conn_auth_info_cb_register(&auth_info_cb);

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_subsys_init();
	}

	int err = bt_enable(bt_ready);

	if (err) {
		printk("bridge/ble: bt_enable err %d\n", err);
		return err;
	}
	return 0;
}
