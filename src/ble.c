/*
 * BLE subsystem — extended advertising and scanning.
 *
 * Transmit: builds a manufacturer-data payload with random sound/delay
 * assignments for every device, then broadcasts it as a BLE extended
 * advertisement (5 events over ~150 ms).
 *
 * Receive: passively scans for matching broadcasts, parses this
 * device's assignment, and schedules playback on a dedicated work
 * queue so the BLE thread is never blocked.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "ble.h"
#include "audio.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Packet format                                                      */
/* ------------------------------------------------------------------ */

/* 0xFFFF = Bluetooth SIG reserved for testing; replace with a real   */
/* company ID if deployed publicly.                                   */
#define COMPANY_ID_LO  0xFF
#define COMPANY_ID_HI  0xFF
#define MAGIC_BYTE     0x42

#define ASSIGNMENT_ENTRY_SIZE  4   /* device_id(1) + sound(1) + delay(2) */
#define HEADER_SIZE            3   /* company_id(2) + magic(1) */

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                         */
/* ------------------------------------------------------------------ */

#define NUM_SOUNDS    10
#define DELAY_MIN_MS  0
#define DELAY_MAX_MS  2000

#define MAX_DEVICES   10

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static uint8_t              local_device_id;
static const uint8_t       *known_ids;
static uint8_t              num_known;
static const volatile bool *am_triggering_ptr;

static struct bt_le_ext_adv *adv_set;

#define MFG_DATA_MAX_SIZE (HEADER_SIZE + MAX_DEVICES * ASSIGNMENT_ENTRY_SIZE)
static uint8_t mfg_data[MFG_DATA_MAX_SIZE];
static uint8_t mfg_data_size;

/* ------------------------------------------------------------------ */
/* RX playback work queue — offloads sound playback from BLE thread   */
/* ------------------------------------------------------------------ */

static K_THREAD_STACK_DEFINE(rx_wq_stack, 4096);
static struct k_work_q rx_work_q;
static struct k_work_delayable rx_playback_work;
static uint8_t rx_sound_index;

static void rx_playback_handler(struct k_work *work)
{
	if (audio_is_playing()) {
		LOG_INF("RX playback dropped — sound already playing");
		return;
	}
	audio_play_sound(rx_sound_index);
}

/* ------------------------------------------------------------------ */
/* Advertising (transmit)                                             */
/* ------------------------------------------------------------------ */

static void build_mfg_data(void)
{
	mfg_data[0] = COMPANY_ID_LO;
	mfg_data[1] = COMPANY_ID_HI;
	mfg_data[2] = MAGIC_BYTE;

	for (int i = 0; i < num_known; i++) {
		int offset = HEADER_SIZE + i * ASSIGNMENT_ENTRY_SIZE;

		uint8_t  sound = (uint8_t)(sys_rand32_get() % NUM_SOUNDS);
		uint16_t delay = DELAY_MIN_MS +
			(uint16_t)(sys_rand32_get() %
				   (DELAY_MAX_MS - DELAY_MIN_MS + 1));

		mfg_data[offset + 0] = known_ids[i];
		mfg_data[offset + 1] = sound;
		sys_put_le16(delay, &mfg_data[offset + 2]);
	}
}

int ble_advertise_assignments(void)
{
	build_mfg_data();

	struct bt_data ad[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, mfg_data_size),
	};

	int ret = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad),
					 NULL, 0);
	if (ret) {
		LOG_ERR("Failed to set adv data: %d", ret);
		return ret;
	}

	/*
	 * Send 5 advertising events.  With the 20-30 ms advertising
	 * interval configured below, that spans ~100-150 ms total.
	 * Timeout of 200 ms gives margin for scheduling jitter.
	 */
	struct bt_le_ext_adv_start_param start = {
		.timeout = 20,    /* 20 x 10 ms = 200 ms */
		.num_events = 5,
	};

	ret = bt_le_ext_adv_start(adv_set, &start);
	if (ret) {
		LOG_ERR("Failed to start adv: %d", ret);
		return ret;
	}

	LOG_INF("BLE assignment broadcast started");
	return 0;
}

void ble_get_self_assignment(int device_index,
			     uint8_t *sound, uint16_t *delay_ms)
{
	int off = HEADER_SIZE + device_index * ASSIGNMENT_ENTRY_SIZE;
	*sound    = mfg_data[off + 1];
	*delay_ms = sys_get_le16(&mfg_data[off + 2]);
}

/* ------------------------------------------------------------------ */
/* Scanning (receive)                                                 */
/* ------------------------------------------------------------------ */

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
			 struct net_buf_simple *buf)
{
	/*
	 * Ignore broadcasts while we are the trigger source.  Our own
	 * assignment is handled directly in the vibration thread, so
	 * processing our own reflected advertisement would double-play.
	 */
	if (*am_triggering_ptr) {
		return;
	}

	/* Drop RX events while a sound is already playing. */
	if (audio_is_playing()) {
		return;
	}

	if (buf->len < HEADER_SIZE + 2) {
		return;
	}

	struct net_buf_simple_state state;
	net_buf_simple_save(buf, &state);

	while (buf->len > 1) {
		uint8_t ad_len = net_buf_simple_pull_u8(buf);
		if (ad_len == 0 || ad_len > buf->len) {
			break;
		}

		uint8_t ad_type = net_buf_simple_pull_u8(buf);
		uint8_t data_len = ad_len - 1;

		if (ad_type != BT_DATA_MANUFACTURER_DATA ||
		    data_len != mfg_data_size) {
			if (data_len <= buf->len) {
				net_buf_simple_pull(buf, data_len);
			}
			continue;
		}

		const uint8_t *data = buf->data;

		if (data[0] != COMPANY_ID_LO ||
		    data[1] != COMPANY_ID_HI ||
		    data[2] != MAGIC_BYTE) {
			net_buf_simple_pull(buf, data_len);
			continue;
		}

		/* Find our entry in the assignment list */
		for (int i = 0; i < num_known; i++) {
			int off = HEADER_SIZE + i * ASSIGNMENT_ENTRY_SIZE;
			if (data[off] != local_device_id) {
				continue;
			}

			uint8_t  sound = data[off + 1];
			uint16_t delay = sys_get_le16(&data[off + 2]);

			LOG_INF("RX assignment: sound=%u delay=%u ms",
				sound, delay);

			/*
			 * Cancel any pending playback from a prior broadcast
			 * before overwriting rx_sound_index.  Without this,
			 * a rapid second broadcast could overwrite the index
			 * after the first work item was already scheduled but
			 * before it executed, causing the wrong sound to play.
			 */
			k_work_cancel_delayable(&rx_playback_work);

			rx_sound_index = sound;
			k_work_schedule_for_queue(
				&rx_work_q, &rx_playback_work,
				K_MSEC(delay));
			goto done;
		}

		LOG_WRN("Device ID 0x%02x not in broadcast", local_device_id);
		goto done;
	}

done:
	net_buf_simple_restore(buf, &state);
}

static struct bt_le_scan_cb scan_cbs = {
	.recv = scan_recv_cb,
};

/*
 * Scan parameters — power/latency tradeoff:
 *
 * The 50 ms end-to-end latency budget covers:
 *   (1) Time until the scanner's window overlaps the advertisement
 *   (2) Packet reception and callback dispatch (~1 ms)
 *   (3) SPI command to VS1053B to begin playback (~2 ms)
 *
 * Continuous scanning (window == interval) guarantees that any
 * advertisement is detected immediately, making (1) ~ 0 and leaving
 * ~47 ms of headroom for (2) and (3).
 *
 * Cost: ~5-7 mA continuous radio current.  A duty-cycled alternative
 * (e.g. window=30ms / interval=50ms, 60% duty) would save ~40% radio
 * power but risk 20 ms worst-case detection latency, eating into the
 * 50 ms budget.  Since vibration events are rare and the CPU still
 * enters System ON idle between scan events, continuous scanning is
 * the right tradeoff here.
 *
 * 48 units x 0.625 ms = 30 ms, matching the minimum extended
 * advertising interval.  Window == interval == 30 ms = continuous.
 */
#define SCAN_INTERVAL  0x0030
#define SCAN_WINDOW    0x0030

static const struct bt_le_scan_param scan_params = {
	.type = BT_LE_SCAN_TYPE_PASSIVE,
	.options = BT_LE_SCAN_OPT_NONE,
	.interval = SCAN_INTERVAL,
	.window = SCAN_WINDOW,
};

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

int ble_init(uint8_t device_id,
	     const uint8_t *device_ids, uint8_t num_devices,
	     const volatile bool *triggering_flag)
{
	local_device_id   = device_id;
	known_ids         = device_ids;
	num_known         = num_devices;
	am_triggering_ptr = triggering_flag;
	mfg_data_size     = HEADER_SIZE + num_devices * ASSIGNMENT_ENTRY_SIZE;

	/* RX playback work queue with dedicated stack */
	k_work_queue_init(&rx_work_q);
	k_work_queue_start(&rx_work_q, rx_wq_stack,
			   K_THREAD_STACK_SIZEOF(rx_wq_stack),
			   5, NULL);
	k_work_init_delayable(&rx_playback_work, rx_playback_handler);

	int ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return ret;
	}
	LOG_INF("Bluetooth initialized");

	/*
	 * Extended advertising at 20-30 ms intervals.
	 * BT_GAP_ADV_FAST_INT_MIN_2 (100 ms) was too slow — with a
	 * 100 ms timeout only one event would ever fire.  At 20-30 ms,
	 * 5 events fit within ~150 ms, matching the scan window and
	 * giving receivers multiple chances to catch the broadcast.
	 */
	static const struct bt_le_adv_param adv_params =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_EXT_ADV,
				     0x0020,   /* 32 x 0.625 ms = 20 ms */
				     0x0030,   /* 48 x 0.625 ms = 30 ms */
				     NULL);

	ret = bt_le_ext_adv_create(&adv_params, NULL, &adv_set);
	if (ret) {
		LOG_ERR("Failed to create extended adv set: %d", ret);
		return ret;
	}

	bt_le_scan_cb_register(&scan_cbs);

	ret = bt_le_scan_start(&scan_params, NULL);
	if (ret) {
		LOG_ERR("Scanning failed to start: %d", ret);
		return ret;
	}
	LOG_INF("BLE scanning started (interval=%u window=%u x 0.625ms)",
		SCAN_INTERVAL, SCAN_WINDOW);

	return 0;
}
