/*
 * BLE subsystem — extended advertising and scanning.
 *
 * Transmit: serializes caller-provided assignments into a manufacturer-
 * data payload and broadcasts via BLE extended advertising (5 events
 * over ~150 ms).
 *
 * Receive: passively scans for matching broadcasts, parses this
 * device's assignment, and posts an EVENT_BLE_RX to the main event
 * queue.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "ble.h"
#include "events.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Packet format                                                      */
/* ------------------------------------------------------------------ */

/* 0xFFFF = Bluetooth SIG reserved for testing; replace with a real   */
/* company ID if deployed publicly.                                   */
#define COMPANY_ID_LO 0xFF
#define COMPANY_ID_HI 0xFF
#define MAGIC_BYTE 0x42

#define ASSIGNMENT_ENTRY_SIZE 4 /* device_id(1) + sound(1) + delay(2) */
#define HEADER_SIZE 3 /* company_id(2) + magic(1) */

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static uint8_t g_local_device_id;
static struct bt_le_ext_adv *g_adv_set;
static struct k_msgq *g_evt_q;

#define MFG_DATA_MAX_SIZE (HEADER_SIZE + BLE_MAX_ASSIGNMENTS * ASSIGNMENT_ENTRY_SIZE)
static uint8_t g_mfg_data[MFG_DATA_MAX_SIZE];

/* ------------------------------------------------------------------ */
/* Advertising (transmit)                                             */
/* ------------------------------------------------------------------ */

int ble_advertise_assignments(const struct assignment *assignments,
                              uint8_t count)
{
    if (count > BLE_MAX_ASSIGNMENTS) {
        LOG_ERR("Too many assignments (%u > %u)", count,
                BLE_MAX_ASSIGNMENTS);
        return -EINVAL;
    }

    /* Build manufacturer data */
    g_mfg_data[0] = COMPANY_ID_LO;
    g_mfg_data[1] = COMPANY_ID_HI;
    g_mfg_data[2] = MAGIC_BYTE;

    for (int i = 0; i < count; i++) {
        int offset = HEADER_SIZE + i * ASSIGNMENT_ENTRY_SIZE;

        g_mfg_data[offset + 0] = assignments[i].device_id;
        g_mfg_data[offset + 1] = assignments[i].sound;
        sys_put_le16(assignments[i].delay_ms, &g_mfg_data[offset + 2]);
    }

    uint8_t mfg_data_size = HEADER_SIZE + count * ASSIGNMENT_ENTRY_SIZE;

    struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, g_mfg_data, mfg_data_size),
    };

    int ret = bt_le_ext_adv_set_data(g_adv_set, ad, ARRAY_SIZE(ad),
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
        .timeout = 20, /* 20 x 10 ms = 200 ms */
        .num_events = 5,
    };

    ret = bt_le_ext_adv_start(g_adv_set, &start);
    if (ret) {
        LOG_ERR("Failed to start adv: %d", ret);
        return ret;
    }

    LOG_INF("BLE assignment broadcast started (%u entries)", count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scanning (receive)                                                 */
/* ------------------------------------------------------------------ */

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
                         struct net_buf_simple *buf)
{
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

        /* Must be manufacturer data with valid structure */
        if (ad_type != BT_DATA_MANUFACTURER_DATA ||
            data_len < HEADER_SIZE ||
            (data_len - HEADER_SIZE) % ASSIGNMENT_ENTRY_SIZE != 0) {
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
        int num_entries = (data_len - HEADER_SIZE) / ASSIGNMENT_ENTRY_SIZE;
        for (int i = 0; i < num_entries; i++) {
            int off = HEADER_SIZE + i * ASSIGNMENT_ENTRY_SIZE;
            if (data[off] != g_local_device_id) {
                continue;
            }

            uint8_t sound = data[off + 1];
            uint16_t delay = sys_get_le16(&data[off + 2]);

            struct event evt = {
                .type = EVENT_BLE_RX,
                .sound = sound,
                .delay_ms = delay,
            };

            /*
                         * K_NO_WAIT: if the queue is full (main loop
                         * is busy handling a prior event), this event
                         * is silently dropped — which is the desired
                         * behavior.
                         */
            k_msgq_put(g_evt_q, &evt, K_NO_WAIT);
            goto done;
        }

        /* Expected when we receive our own broadcast — self is excluded. */
        LOG_DBG("Device ID 0x%02x not in broadcast", g_local_device_id);
        goto done;
    }

done:
    net_buf_simple_restore(buf, &state);
}

static struct bt_le_scan_cb g_scan_cbs = {
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
#define SCAN_INTERVAL 0x0030
#define SCAN_WINDOW 0x0030

static const struct bt_le_scan_param g_scan_params = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = SCAN_INTERVAL,
    .window = SCAN_WINDOW,
};

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

int ble_init(uint8_t device_id, struct k_msgq *event_q)
{
    g_local_device_id = device_id;
    g_evt_q = event_q;

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
                             0x0020, /* 32 x 0.625 ms = 20 ms */
                             0x0030, /* 48 x 0.625 ms = 30 ms */
                             NULL);

    ret = bt_le_ext_adv_create(&adv_params, NULL, &g_adv_set);
    if (ret) {
        LOG_ERR("Failed to create extended adv set: %d", ret);
        return ret;
    }

    bt_le_scan_cb_register(&g_scan_cbs);

    ret = bt_le_scan_start(&g_scan_params, NULL);
    if (ret) {
        LOG_ERR("Scanning failed to start: %d", ret);
        return ret;
    }
    LOG_INF("BLE scanning started (interval=%u window=%u x 0.625ms)",
            SCAN_INTERVAL, SCAN_WINDOW);

    return 0;
}
