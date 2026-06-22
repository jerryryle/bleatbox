/*
 * BLE subsystem — extended advertising and scanning with relay support.
 *
 * One on-air format: a 5-byte relay header plus a 16-byte message payload
 * (six positional slots).  Transmit broadcasts it via extended advertising
 * (5 events over ~150 ms).  macOS, which can only advertise Service UUIDs,
 * sends the same payload as a 128-bit UUID; the first device to hear it
 * re-emits the manufacturer-data form so the mesh relay carries it onward.
 *
 * Receive: passively scans for matching broadcasts, deduplicates by
 * (originator, seq), plays this device's slot, posts an EVENT_BLE_RX to the
 * main event queue, and relays the packet if the TTL has not expired.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "ble.h"
#include "ble_ota.h"
#include "broadcast_log.h"
#include "device_config.h"
#include "events.h"
#include "message.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Packet format                                                      */
/* ------------------------------------------------------------------ */

/* 0xFFFF = Bluetooth SIG reserved for testing; replace with a real   */
/* company ID if deployed publicly.                                   */
#define COMPANY_ID_LO 0xFF
#define COMPANY_ID_HI 0xFF
#define MAGIC_BYTE 0x42

#define HEADER_SIZE 5 /* company_id(2) + magic(1) + originator(1) + ttl(1) */
#define MESSAGE_PAYLOAD_LEN 16
#define MESSAGE_MFG_SIZE (HEADER_SIZE + MESSAGE_PAYLOAD_LEN)

/* AD structure: 1 byte length + 1 byte type + manufacturer data */
#define BLE_AD_OVERHEAD 2
BUILD_ASSERT(BLE_AD_OVERHEAD + MESSAGE_MFG_SIZE <= CONFIG_BT_CTLR_ADV_DATA_LEN_MAX,
             "message packet exceeds controller advertising data capacity");

/* Offsets within the header */
/* The relay sequence number lives in the payload (MESSAGE_SEQ_OFFSET), not the
 * header, so the mesh and macOS paths carry it identically. */
#define HDR_OFF_COMPANY_LO 0
#define HDR_OFF_COMPANY_HI 1
#define HDR_OFF_MAGIC      2
#define HDR_OFF_ORIGINATOR 3
#define HDR_OFF_TTL        4

/* ------------------------------------------------------------------ */
/* macOS Service-UUID ingress                                         */
/* ------------------------------------------------------------------ */

/* macOS can't send manufacturer data, so it advertises the message payload
 * as a 128-bit Service UUID alongside this fixed 16-bit marker UUID, which
 * identifies the advert as ours.  See message.h. */
#define MESSAGE_MARKER_UUID16 0xFB42

/* CoreBluetooth re-advertises continuously, so the Mac sends a short burst
 * per message (one stable seq).  The (originator, seq) dedup collapses the
 * burst; this guard additionally suppresses re-gatewaying the same seq if
 * the shared dedup ring evicts it mid-burst in a busy mesh. */
#define MESSAGE_DEBOUNCE_MS 4000

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static uint8_t g_local_device_id;
static struct bt_le_ext_adv *g_adv_set;
static struct k_msgq *g_evt_q;
static uint8_t g_relay_ttl;
static volatile bool g_adv_active;

/* Per-trigger relay sequence counter for this device's own broadcasts.
 * Increments (wrapping at MESSAGE_SEQ_MASK) so back-to-back triggers never
 * collide in neighbors' dedup rings.  Dedup still only checks for existence,
 * making no assumption that the counter increases — so a reset that restarts
 * it at 0 is harmless beyond a brief chance of a stale-entry collision. */
static uint16_t g_seq;

/* This device's slot, derived from its id (MESSAGE_NO_SLOT = does not play,
 * but still relays), plus the last macOS-message seq we gatewayed and when,
 * to suppress re-flooding the same continuously-advertised seq. */
static uint8_t g_local_slot;
static uint16_t g_last_uuid_seq;
static int64_t g_last_uuid_time;

/* Set true once ble_init() has enabled the stack and created the adv
 * set; gates ble_start() so it never scans on an uninitialized stack. */
static bool g_ble_ready;

/* True while the mesh is paused (e.g. during an OTA window), so the BLE
 * controller is free for connectable advertising.  Gates scan restarts. */
static volatile bool g_paused;

#define MFG_DATA_MAX_SIZE MESSAGE_MFG_SIZE
static uint8_t g_mfg_data[MFG_DATA_MAX_SIZE];

/*
 * Relay buffer — written by the scan callback (BT RX workqueue),
 * consumed by the relay work item.  Guarded by g_relay_lock, NOT
 * g_tx_mutex: the mutex is held across blocking HCI commands, and the
 * scan callback must never wait behind those or it would stall the
 * host's event processing.
 */
static struct k_spinlock g_relay_lock;
static uint8_t g_relay_buf[MFG_DATA_MAX_SIZE];
static uint8_t g_relay_len;
static uint8_t g_relay_new_ttl;

/* Serializes the transmit paths (local broadcast, relay, scan resume). */
static K_MUTEX_DEFINE(g_tx_mutex);

/*
 * Dedicated workqueue for relay and scan-resume work.  Both issue
 * blocking HCI commands and may wait on g_tx_mutex — running them on
 * the system workqueue would stall unrelated work items, and the BT
 * RX workqueue (where the callbacks fire) must stay unblocked.
 */
#define TX_WORK_Q_STACK_SIZE 2048
#define TX_WORK_Q_PRIORITY 7
static K_THREAD_STACK_DEFINE(g_tx_work_q_stack, TX_WORK_Q_STACK_SIZE);
static struct k_work_q g_tx_work_q;
static struct k_work g_relay_work;
static struct k_work g_resume_scan_work;

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
    LOG_INF("BLE advertising done (%u events)", info->num_sent);

    /* Fires on the BT RX workqueue — defer the blocking scan restart. */
    k_work_submit_to_queue(&g_tx_work_q, &g_resume_scan_work);
}

/*
 * (Re)start scanning.  -EALREADY (scan never stopped) is not an
 * error.  Used after a burst completes and on every advertising
 * failure path — scanning is this device's only way to hear peers,
 * so no path may leave it off without a pending burst to restore it.
 */
static void resume_scanning(void)
{
    /* While paused (OTA window), the controller is reserved for connectable
     * advertising — don't let a completing adv burst restart the scan. */
    if (g_paused) {
        return;
    }

    int ret = bt_le_scan_start(&g_scan_params, NULL);
    if (ret && ret != -EALREADY) {
        LOG_ERR("Failed to restart scanning: %d", ret);
    }
}

static void resume_scan_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    k_mutex_lock(&g_tx_mutex, K_FOREVER);

    /* A relay may have started another burst since the sent event —
     * its own completion will resume scanning. */
    if (!g_adv_active) {
        resume_scanning();
        LOG_INF("BLE scan resumed");
    }

    k_mutex_unlock(&g_tx_mutex);
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
        /* Scanning may already be off (preempted relay burst). */
        resume_scanning();
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
        resume_scanning();
        return ret;
    }

    g_adv_active = true;
    return 0;
}

int ble_broadcast_message(const uint8_t payload[16])
{
    k_mutex_lock(&g_tx_mutex, K_FOREVER);

    /*
     * A relay burst may be in flight.  This fresh local message takes
     * priority — stop the relay (which is best-effort) rather than
     * failing with -EALREADY.
     */
    if (g_adv_active) {
        int err = bt_le_ext_adv_stop(g_adv_set);
        if (err) {
            /* Burst still running; starting over it would fail with
             * -EALREADY and the scan-restart path would overlap scan
             * with advertising.  Bail — the running burst's completion
             * still resumes scanning. */
            LOG_WRN("Failed to stop in-flight adv: %d", err);
            k_mutex_unlock(&g_tx_mutex);
            return err;
        }
        LOG_INF("Preempted in-flight relay broadcast");
        g_adv_active = false;
    }

    uint16_t seq = g_seq;
    g_seq = (uint16_t)((g_seq + 1) & MESSAGE_SEQ_MASK);

    /* Record in dedup table so we don't process our own broadcast */
    broadcast_log_record(g_local_device_id, seq);

    g_mfg_data[HDR_OFF_COMPANY_LO] = COMPANY_ID_LO;
    g_mfg_data[HDR_OFF_COMPANY_HI] = COMPANY_ID_HI;
    g_mfg_data[HDR_OFF_MAGIC] = MAGIC_BYTE;
    g_mfg_data[HDR_OFF_ORIGINATOR] = g_local_device_id;
    g_mfg_data[HDR_OFF_TTL] = g_relay_ttl;
    memcpy(&g_mfg_data[HEADER_SIZE], payload, MESSAGE_PAYLOAD_LEN);
    /* The seq rides in the payload now, so stamp it after copying. */
    message_set_seq(&g_mfg_data[HEADER_SIZE], seq);

    int ret = start_advertising(MESSAGE_MFG_SIZE);
    k_mutex_unlock(&g_tx_mutex);

    if (ret) {
        return ret;
    }

    LOG_INF("BLE broadcast: originator=0x%02x seq=%u ttl=%u",
            g_local_device_id, seq, g_relay_ttl);
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

    k_spinlock_key_t key = k_spin_lock(&g_relay_lock);
    uint8_t len = g_relay_len;
    uint8_t originator = g_relay_buf[HDR_OFF_ORIGINATOR];
    uint16_t seq = message_get_seq(&g_relay_buf[HEADER_SIZE]);
    uint8_t ttl = g_relay_new_ttl;
    memcpy(g_mfg_data, g_relay_buf, len);
    g_mfg_data[HDR_OFF_TTL] = ttl;
    k_spin_unlock(&g_relay_lock, key);

    LOG_INF("Relaying: originator=0x%02x seq=%u ttl=%u",
            originator, seq, ttl);

    int ret = start_advertising(len);
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

    /* Runs in the scan callback (BT RX workqueue) — must not block. */
    k_spinlock_key_t key = k_spin_lock(&g_relay_lock);
    memcpy(g_relay_buf, data, data_len);
    g_relay_len = data_len;
    g_relay_new_ttl = ttl;
    k_spin_unlock(&g_relay_lock, key);

    int ret = k_work_submit_to_queue(&g_tx_work_q, &g_relay_work);
    if (ret < 0) {
        LOG_ERR("Relay work submit failed: %d", ret);
    } else if (ret == 0) {
        LOG_DBG("Relay already pending — previous relay superseded");
    }
}

static void post_play_event(uint8_t encoded_sound, uint16_t delay_ms)
{
    struct event evt = {
        .type = EVENT_BLE_RX,
        .sound = encoded_sound,
        .delay_ms = delay_ms,
    };
    k_msgq_put(g_evt_q, &evt, K_NO_WAIT);
}

/*
 * Act on a 16-byte message payload — same handling for the mesh and macOS
 * ingress paths.  An OTA-arm command opens the update window; a play command
 * plays this device's slot (a slotless or silent device stays quiet).
 *
 * Returns true if the message was an OTA-arm command.  OTA commands must NOT
 * be relayed/gatewayed: relaying is an extended-advertising burst, which
 * disrupts the connectable advertising the OTA window relies on — and it buys
 * nothing, since the SMP upload is one box at a time within radio range anyway.
 */
static bool act_on_message(const uint8_t *payload)
{
    if (ble_ota_is_arm(payload)) {
        LOG_INF("OTA arm command");
        struct event evt = {.type = EVENT_OTA_ARM};
        k_msgq_put(g_evt_q, &evt, K_NO_WAIT);
        return true;
    }

    if (message_get_command(payload) != MESSAGE_CMD_PLAY) {
        return false;
    }

    struct message_slot s;
    message_parse_slot(payload, g_local_slot, &s);
    if (!s.play) {
        return false;
    }

    post_play_event(ble_sound_encode((enum sound_type)s.type, s.sound),
                    s.delay_ms);
    LOG_INF("Message play: slot=%u type=%u sound=%u delay=%u ms",
            g_local_slot, s.type, s.sound, s.delay_ms);
    return false;
}

/*
 * Shared core of both ingress paths: dedup on (originator, payload seq), and
 * if the message is new, act on it locally.  @p out_is_ota reports whether it
 * was an OTA-arm command (which must not be propagated).
 *
 * @return true if the message is new (caller should propagate it), false if it
 *         was a duplicate (already handled).
 */
static bool dedup_and_act(uint8_t originator, const uint8_t *payload,
                          bool *out_is_ota)
{
    uint16_t seq = message_get_seq(payload);

    if (broadcast_log_check_and_record(originator, seq)) {
        LOG_DBG("Duplicate dropped: originator=0x%02x seq=%u", originator, seq);
        return false;
    }

    *out_is_ota = act_on_message(payload);
    return true;
}

static void handle_mfg_data(const uint8_t *data, uint8_t data_len)
{
    if (data_len != MESSAGE_MFG_SIZE) {
        return;
    }

    if (data[HDR_OFF_COMPANY_LO] != COMPANY_ID_LO ||
        data[HDR_OFF_COMPANY_HI] != COMPANY_ID_HI ||
        data[HDR_OFF_MAGIC] != MAGIC_BYTE) {
        return;
    }

    /* The TTL arrives from the air — clamp it to our own configured
     * limit so a spoofed packet can't make the mesh relay 255 hops. */
    uint8_t ttl = MIN(data[HDR_OFF_TTL], g_relay_ttl);

    bool is_ota;
    if (!dedup_and_act(data[HDR_OFF_ORIGINATOR], &data[HEADER_SIZE], &is_ota)) {
        return;
    }

    if (ttl > 0 && !is_ota) {
        schedule_relay(data, data_len, ttl - 1);
    }
}

/* True if the 16-bit Service UUID list contains @p uuid. */
static bool uuid16_list_has(const uint8_t *data, uint8_t len, uint16_t uuid)
{
    for (uint8_t i = 0; i + 2 <= len; i += 2) {
        if (sys_get_le16(&data[i]) == uuid) {
            return true;
        }
    }
    return false;
}

/*
 * A macOS-originated message arrived as a Service-UUID advert.  This is the
 * ingress: play our own slot and re-emit it as a manufacturer-data packet so
 * the existing relay path floods it across the mesh.  A slotless device does
 * not play but still gateways, bridging the message to devices out of the
 * Mac's radio range.
 */
static void handle_message_uuid(const uint8_t *payload)
{
    uint16_t seq = message_get_seq(payload);

    /* The Mac advertises one stable seq per burst.  Suppress re-gatewaying
     * it even if the shared dedup ring evicts (0xFE, seq) mid-burst in a
     * busy mesh. */
    int64_t now = k_uptime_get();
    if (g_last_uuid_time != 0 && seq == g_last_uuid_seq &&
        now - g_last_uuid_time < MESSAGE_DEBOUNCE_MS) {
        return;
    }

    /* Same dedup + local handling as the mesh path, keyed on the reserved
     * macOS originator.  If a neighbor's relay of this message already reached
     * us, don't gateway it again. */
    bool is_ota;
    if (!dedup_and_act(MESSAGE_EXT_ORIGINATOR, payload, &is_ota)) {
        return;
    }

    g_last_uuid_seq = seq;
    g_last_uuid_time = now;

    /* OTA commands are acted on locally but never gatewayed into the mesh —
     * the relay burst would disrupt this box's own OTA connectable window. */
    if (is_ota) {
        return;
    }

    /* Re-emit as a manufacturer-data packet so the relay path floods it.  The
     * seq already rides in the payload, so the header carries no seq byte. */
    uint8_t pkt[MESSAGE_MFG_SIZE];
    pkt[HDR_OFF_COMPANY_LO] = COMPANY_ID_LO;
    pkt[HDR_OFF_COMPANY_HI] = COMPANY_ID_HI;
    pkt[HDR_OFF_MAGIC] = MAGIC_BYTE;
    pkt[HDR_OFF_ORIGINATOR] = MESSAGE_EXT_ORIGINATOR;
    pkt[HDR_OFF_TTL] = g_relay_ttl; /* relay handler stamps the real ttl */
    memcpy(&pkt[HEADER_SIZE], payload, MESSAGE_PAYLOAD_LEN);

    LOG_INF("macOS message seq=%u — flooding mesh (ttl=%u)", seq, g_relay_ttl);
    schedule_relay(pkt, sizeof(pkt), g_relay_ttl);
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
                         struct net_buf_simple *buf)
{
    if (buf->len < 2) {
        return;
    }

    struct net_buf_simple_state state;
    net_buf_simple_save(buf, &state);

    bool handled_mfg = false;
    bool marker_seen = false;
    const uint8_t *uuid_payload = NULL;

    while (buf->len > 1) {
        uint8_t ad_len = net_buf_simple_pull_u8(buf);
        if (ad_len == 0 || ad_len > buf->len) {
            break;
        }

        uint8_t ad_type = net_buf_simple_pull_u8(buf);
        uint8_t data_len = ad_len - 1;
        if (data_len > buf->len) {
            break;
        }

        const uint8_t *data = buf->data;

        /* A packet is either a mesh broadcast (manufacturer data) or a
         * macOS message (Service UUIDs), never both, so handle and stop. */
        if (ad_type == BT_DATA_MANUFACTURER_DATA &&
            data_len == MESSAGE_MFG_SIZE) {
            handle_mfg_data(data, data_len);
            handled_mfg = true;
            break;
        }

        if ((ad_type == BT_DATA_UUID16_ALL || ad_type == BT_DATA_UUID16_SOME) &&
            uuid16_list_has(data, data_len, MESSAGE_MARKER_UUID16)) {
            marker_seen = true;
        } else if ((ad_type == BT_DATA_UUID128_ALL ||
                    ad_type == BT_DATA_UUID128_SOME) &&
                   data_len == MESSAGE_PAYLOAD_LEN) {
            uuid_payload = data;
        }

        net_buf_simple_pull(buf, data_len);
    }

    if (!handled_mfg && marker_seen && uuid_payload != NULL) {
        handle_message_uuid(uuid_payload);
    }

    net_buf_simple_restore(buf, &state);
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

int ble_init(struct k_msgq *event_q)
{
    g_evt_q = event_q;
    g_adv_active = false;
    g_seq = 0;
    g_relay_len = 0;
    g_relay_new_ttl = 0;
    g_ble_ready = false;

    /* Real values arrive in ble_start(); scanning is off until then,
     * so the callbacks that read these never run with the defaults. */
    g_local_device_id = 0;
    g_relay_ttl = 0;
    g_local_slot = MESSAGE_NO_SLOT;
    g_last_uuid_seq = 0;
    g_last_uuid_time = 0;
    g_paused = false;

    broadcast_log_init();
    k_work_init(&g_relay_work, relay_work_handler);
    k_work_init(&g_resume_scan_work, resume_scan_work_handler);

    k_work_queue_init(&g_tx_work_q);
    k_work_queue_start(&g_tx_work_q, g_tx_work_q_stack,
                       K_THREAD_STACK_SIZEOF(g_tx_work_q_stack),
                       TX_WORK_Q_PRIORITY, NULL);

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

    g_ble_ready = true;
    return 0;
}

int ble_start(uint8_t device_id, uint8_t relay_ttl)
{
    if (!g_ble_ready) {
        return -ENODEV;
    }

    g_local_device_id = device_id;
    g_relay_ttl = relay_ttl;
    g_local_slot = message_slot_for_id(device_id);

    int ret = bt_le_scan_start(&g_scan_params, NULL);
    if (ret) {
        LOG_ERR("Scanning failed to start: %d", ret);
        return ret;
    }
    LOG_INF("BLE scanning started (interval=%u window=%u x 0.625ms)",
            SCAN_INTERVAL, SCAN_WINDOW);
    LOG_INF("BLE relay TTL: %u", g_relay_ttl);
    if (g_local_slot == MESSAGE_NO_SLOT) {
        LOG_INF("BLE slot: none (relay only)");
    } else {
        LOG_INF("BLE slot: %u", g_local_slot);
    }

    return 0;
}

void ble_pause(void)
{
    if (!g_ble_ready) {
        return;
    }

    /* Stop scanning and hold any in-flight burst's completion from
     * restarting it, so the controller is free for connectable advertising
     * (the OTA window).  Mesh transmits are already quiet during OTA: the
     * accelerometer trigger is gated and OTA commands aren't relayed. */
    g_paused = true;
    bt_le_scan_stop();
    LOG_INF("BLE mesh paused");
}

void ble_resume(void)
{
    if (!g_ble_ready) {
        return;
    }

    g_paused = false;
    bt_le_scan_start(&g_scan_params, NULL);
    LOG_INF("BLE mesh resumed");
}
