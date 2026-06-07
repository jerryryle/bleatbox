/*
 * BLE subsystem — extended advertising and scanning with relay support.
 *
 * Transmit: serializes caller-provided assignments into a manufacturer-
 * data payload and broadcasts via BLE extended advertising (5 events
 * over ~150 ms).
 *
 * Receive: passively scans for matching broadcasts, deduplicates by
 * (originator, seq), parses this device's assignment, posts an
 * EVENT_BLE_RX to the main event queue, and relays the packet if
 * the TTL has not expired.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "ble.h"
#include "broadcast_log.h"
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
#define HEADER_SIZE 6 /* company_id(2) + magic(1) + originator(1) + seq(1) + ttl(1) */

/* AD structure: 1 byte length + 1 byte type + manufacturer data */
#define BLE_AD_OVERHEAD 2
BUILD_ASSERT(BLE_AD_OVERHEAD + HEADER_SIZE + BLE_MAX_ASSIGNMENTS * ASSIGNMENT_ENTRY_SIZE
             <= CONFIG_BT_CTLR_ADV_DATA_LEN_MAX,
             "BLE_MAX_ASSIGNMENTS exceeds controller advertising data capacity");

/* Offsets within the header */
#define HDR_OFF_COMPANY_LO 0
#define HDR_OFF_COMPANY_HI 1
#define HDR_OFF_MAGIC      2
#define HDR_OFF_ORIGINATOR 3
#define HDR_OFF_SEQ        4
#define HDR_OFF_TTL        5

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static uint8_t g_local_device_id;
static struct bt_le_ext_adv *g_adv_set;
static struct k_msgq *g_evt_q;
static uint8_t g_relay_ttl;
static uint8_t g_seq;
static volatile bool g_adv_active;

#define MFG_DATA_MAX_SIZE (HEADER_SIZE + BLE_MAX_ASSIGNMENTS * ASSIGNMENT_ENTRY_SIZE)
static uint8_t g_mfg_data[MFG_DATA_MAX_SIZE];

static uint8_t g_relay_buf[MFG_DATA_MAX_SIZE];
static uint8_t g_relay_len;
static uint8_t g_relay_new_ttl;
static struct k_work g_relay_work;
static K_MUTEX_DEFINE(g_tx_mutex);

/*
 * Scan parameters — 100 ms window inside a 160 ms interval (~62%
 * duty cycle).  The advertising burst is 5 events over ~150 ms, so
 * a 100 ms window reliably catches at least one event per burst.
 */
#define SCAN_INTERVAL 0x0100 /* 256 × 0.625 ms = 160 ms */
#define SCAN_WINDOW   0x00A0 /* 160 × 0.625 ms = 100 ms */

static const struct bt_le_scan_param g_scan_params = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = SCAN_INTERVAL,
    .window = SCAN_WINDOW,
};

static void adv_sent_cb(struct bt_le_ext_adv *adv,
                         struct bt_le_ext_adv_sent_info *info);
static const struct bt_le_ext_adv_cb g_adv_cbs = {
    .sent = adv_sent_cb,
};

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
                         struct net_buf_simple *buf);
static struct bt_le_scan_cb g_scan_cbs = {
    .recv = scan_recv_cb,
};

/* ------------------------------------------------------------------ */
/* Advertising (transmit)                                             */
/* ------------------------------------------------------------------ */

static void adv_sent_cb(struct bt_le_ext_adv *adv,
                         struct bt_le_ext_adv_sent_info *info)
{
    ARG_UNUSED(adv);
    g_adv_active = false;
    LOG_INF("BLE advertising done (%u events), resuming scan",
            info->num_sent);
    int ret = bt_le_scan_start(&g_scan_params, NULL);
    if (ret) {
        LOG_ERR("Failed to restart scanning: %d", ret);
    }
}

static int start_advertising(uint8_t mfg_data_size)
{
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
     * Stop scanning while advertising to avoid a timing conflict
     * in the Zephyr BLE controller's ticker when extended
     * advertising and continuous scanning run concurrently.
     */
    LOG_INF("Stopping BLE scan to start advertising");
    bt_le_scan_stop();

    struct bt_le_ext_adv_start_param start = {
        .timeout = 20, /* 20 x 10 ms = 200 ms */
        .num_events = 5,
    };

    ret = bt_le_ext_adv_start(g_adv_set, &start);
    if (ret) {
        LOG_ERR("Failed to start adv: %d", ret);
        bt_le_scan_start(&g_scan_params, NULL);
        return ret;
    }

    g_adv_active = true;
    return 0;
}

int ble_advertise_assignments(const struct assignment *assignments,
                              uint8_t count)
{
    if (count > BLE_MAX_ASSIGNMENTS) {
        LOG_ERR("Too many assignments (%u > %u)", count,
                BLE_MAX_ASSIGNMENTS);
        return -EINVAL;
    }

    k_mutex_lock(&g_tx_mutex, K_FOREVER);

    uint8_t seq = g_seq++;

    /* Record in dedup table so we don't process our own broadcast */
    broadcast_log_record(g_local_device_id, seq);

    /* Build header */
    g_mfg_data[HDR_OFF_COMPANY_LO] = COMPANY_ID_LO;
    g_mfg_data[HDR_OFF_COMPANY_HI] = COMPANY_ID_HI;
    g_mfg_data[HDR_OFF_MAGIC] = MAGIC_BYTE;
    g_mfg_data[HDR_OFF_ORIGINATOR] = g_local_device_id;
    g_mfg_data[HDR_OFF_SEQ] = seq;
    g_mfg_data[HDR_OFF_TTL] = g_relay_ttl;

    /* Build assignment entries */
    for (int i = 0; i < count; i++) {
        int offset = HEADER_SIZE + i * ASSIGNMENT_ENTRY_SIZE;

        g_mfg_data[offset + 0] = assignments[i].device_id;
        g_mfg_data[offset + 1] = ble_sound_encode(SOUND_TYPE_GOAT,
                                                     assignments[i].sound);
        sys_put_le16(assignments[i].delay_ms, &g_mfg_data[offset + 2]);
    }

    uint8_t mfg_data_size = HEADER_SIZE + count * ASSIGNMENT_ENTRY_SIZE;

    int ret = start_advertising(mfg_data_size);
    k_mutex_unlock(&g_tx_mutex);

    if (ret) {
        return ret;
    }

    LOG_INF("BLE broadcast: originator=0x%02x seq=%u ttl=%u entries=%u",
            g_local_device_id, seq, g_relay_ttl, count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scanning (receive)                                                 */
/* ------------------------------------------------------------------ */

static void relay_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    k_mutex_lock(&g_tx_mutex, K_FOREVER);

    if (g_adv_active) {
        k_mutex_unlock(&g_tx_mutex);
        LOG_DBG("Relay skipped — advertising already active");
        return;
    }

    memcpy(g_mfg_data, g_relay_buf, g_relay_len);
    g_mfg_data[HDR_OFF_TTL] = g_relay_new_ttl;
    LOG_INF("Relaying: originator=0x%02x seq=%u ttl=%u",
            g_relay_buf[HDR_OFF_ORIGINATOR], g_relay_buf[HDR_OFF_SEQ],
            g_relay_new_ttl);

    int ret = start_advertising(g_relay_len);
    k_mutex_unlock(&g_tx_mutex);

    if (ret) {
        LOG_WRN("Relay failed: %d", ret);
    }
}

static void schedule_relay(const uint8_t *data, uint8_t data_len, uint8_t ttl)
{
    if (data_len > MFG_DATA_MAX_SIZE) {
        LOG_WRN("Relay skipped — packet too large (%u > %u)",
                data_len, MFG_DATA_MAX_SIZE);
        return;
    }

    k_mutex_lock(&g_tx_mutex, K_FOREVER);
    memcpy(g_relay_buf, data, data_len);
    g_relay_len = data_len;
    g_relay_new_ttl = ttl;
    k_mutex_unlock(&g_tx_mutex);

    int ret = k_work_submit(&g_relay_work);
    if (ret < 0) {
        LOG_ERR("Relay work submit failed: %d", ret);
    } else if (ret == 0) {
        LOG_DBG("Relay already pending — previous relay superseded");
    }
}

static void handle_mfg_data(const uint8_t *data, uint8_t data_len)
{
    if (data_len < HEADER_SIZE) {
        return;
    }

    if (data[HDR_OFF_COMPANY_LO] != COMPANY_ID_LO ||
        data[HDR_OFF_COMPANY_HI] != COMPANY_ID_HI ||
        data[HDR_OFF_MAGIC] != MAGIC_BYTE) {
        return;
    }

    uint8_t originator = data[HDR_OFF_ORIGINATOR];
    uint8_t seq = data[HDR_OFF_SEQ];
    uint8_t ttl = data[HDR_OFF_TTL];

    if (broadcast_log_check_and_record(originator, seq)) {
        LOG_DBG("Duplicate dropped: originator=0x%02x seq=%u",
                originator, seq);
        return;
    }

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

        k_msgq_put(g_evt_q, &evt, K_NO_WAIT);
        break;
    }

    if (ttl > 0) {
        schedule_relay(data, data_len, ttl - 1);
    }
}

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

        if (ad_type != BT_DATA_MANUFACTURER_DATA ||
            data_len < HEADER_SIZE ||
            data_len > buf->len ||
            (data_len - HEADER_SIZE) % ASSIGNMENT_ENTRY_SIZE != 0) {
            if (data_len <= buf->len) {
                net_buf_simple_pull(buf, data_len);
            }
            continue;
        }

        handle_mfg_data(buf->data, data_len);
        break;
    }

    net_buf_simple_restore(buf, &state);
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

int ble_init(uint8_t device_id, struct k_msgq *event_q, uint8_t relay_ttl)
{
    g_local_device_id = device_id;
    g_evt_q = event_q;
    g_relay_ttl = relay_ttl;
    g_seq = 0;
    g_adv_active = false;
    broadcast_log_init();
    k_work_init(&g_relay_work, relay_work_handler);

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

    ret = bt_le_ext_adv_create(&adv_params, &g_adv_cbs, &g_adv_set);
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
    LOG_INF("BLE relay TTL: %u", g_relay_ttl);

    return 0;
}
