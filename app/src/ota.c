/*
 * OTA update window (main-app side).
 *
 * Opens a connectable SMP-over-BLE window on command so an operator can
 * upload a firmware image to the SD card with mcumgr, then reboots to let
 * the second-stage updater apply it.  See ota.h and the second-stage
 * updater under updater/.
 *
 * The mesh (ble.c) keeps running during the window: OTA commands flood the
 * swarm through the existing TTL relay, and this module only adds a second,
 * connectable advertising set for SMP.  Concurrent connectable advertising
 * alongside the mesh's extended advertising + scanning can only be
 * validated on hardware; `mcumgr reset` is a per-box fallback for commit.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#include "ble.h"
#include "ota.h"
#include "events.h"

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

/* How long a window stays open before auto-closing. */
#define OTA_WINDOW_MS (5 * 60 * 1000)

static struct k_msgq *g_evt_q;
static uint8_t g_device_id;
static bool g_ready;
static volatile bool g_active;
static char g_dev_name[24];

/* Connectable advertising set for SMP.  Created once, on first use. */
static struct bt_le_ext_adv *g_conn_adv;

static void window_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(g_window_timer, window_expiry, NULL);

static void readvertise_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_readvertise_work, readvertise_work_handler);

/*
 * Connectable advertising for the OTA window.  Uses the *extended* advertising
 * API (like the mesh) to avoid mixing it with the legacy API under one
 * controller — but with legacy PDUs (no BT_LE_ADV_OPT_EXT_ADV) so macOS
 * CoreBluetooth can still discover and connect to it.
 */
static const struct bt_le_adv_param g_conn_params = BT_LE_ADV_PARAM_INIT(
    BT_LE_ADV_OPT_CONN,
    BT_GAP_ADV_FAST_INT_MIN_2, /* 100 ms */
    BT_GAP_ADV_FAST_INT_MAX_2, /* 150 ms */
    NULL);

/*
 * The window timer fires on the system workqueue; hand the close-up back to
 * the main loop (which owns ota_cancel) rather than touching BLE here.
 */
static void window_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    struct event evt = {.type = EVENT_OTA_CANCEL};
    k_msgq_put(g_evt_q, &evt, K_NO_WAIT);
}

/*
 * Restart the connectable advertising a connection stopped.  Deferred from the
 * disconnected callback: the closed connection object isn't freed until that
 * callback returns, so restarting connectable advertising there fails with
 * -ENOMEM (no free connection).  A short delay lets it free first.
 */
static void readvertise_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!g_active || g_conn_adv == NULL) {
        return;
    }

    int err = bt_le_ext_adv_start(g_conn_adv, BT_LE_EXT_ADV_START_DEFAULT);
    if (err) {
        LOG_WRN("OTA re-advertise failed: %d", err);
    } else {
        LOG_INF("OTA re-advertising after disconnect");
    }
}

/*
 * A connection stops the connectable advertising.  While the OTA window is
 * still open, restart it on disconnect so the operator can reconnect — e.g.
 * after `file upload` to send `os reset`, which each open a fresh connection.
 */
static void ota_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(reason);

    if (!g_active) {
        return;
    }

    k_work_schedule(&g_readvertise_work, K_MSEC(100));
}

static struct bt_conn_cb g_ota_conn_cb = {
    .disconnected = ota_disconnected,
};

int ota_init(struct k_msgq *event_q)
{
    g_evt_q = event_q;
    g_device_id = 0;
    g_ready = false;
    g_active = false;
    g_conn_adv = NULL;
    g_dev_name[0] = '\0';

    bt_conn_cb_register(&g_ota_conn_cb);

    /* BT is enabled by ble_init(); SMP-over-BLE registers its GATT service
     * automatically via SYS_INIT.  Nothing to bring up here. */
    g_ready = true;
    return 0;
}

void ota_set_device_id(uint8_t device_id)
{
    g_device_id = device_id;
}

void ota_start(void)
{
    if (!g_ready) {
        return;
    }

    /* Re-arming an open window just refreshes the timeout. */
    if (g_active) {
        k_timer_start(&g_window_timer, K_MSEC(OTA_WINDOW_MS), K_NO_WAIT);
        return;
    }

    snprintf(g_dev_name, sizeof(g_dev_name), "bleatbox-dfu-%u", g_device_id);

    /* Set the GAP device name too, not just the advertised local name —
     * macOS caches and prefers the GAP name after connecting, so without this
     * the box shows up as the default "Zephyr". */
    int name_err = bt_set_name(g_dev_name);
    if (name_err) {
        LOG_WRN("Failed to set BT device name: %d", name_err);
    }

    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
        BT_DATA(BT_DATA_NAME_COMPLETE, g_dev_name, strlen(g_dev_name)),
    };

    /* Give the connectable advertisement the controller to itself —
     * continuous mesh scanning otherwise contends with it. */
    ble_pause();

    int err;
    if (g_conn_adv == NULL) {
        err = bt_le_ext_adv_create(&g_conn_params, NULL, &g_conn_adv);
        if (err) {
            LOG_ERR("OTA adv set create failed: %d", err);
            ble_resume();
            return;
        }
    }

    err = bt_le_ext_adv_set_data(g_conn_adv, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("OTA adv data failed: %d", err);
        ble_resume();
        return;
    }

    err = bt_le_ext_adv_start(g_conn_adv, BT_LE_EXT_ADV_START_DEFAULT);
    if (err) {
        LOG_ERR("OTA connectable advertising failed: %d", err);
        ble_resume();
        return;
    }

    g_active = true;
    k_timer_start(&g_window_timer, K_MSEC(OTA_WINDOW_MS), K_NO_WAIT);
    LOG_INF("OTA window open as '%s' — mcumgr fs upload now", g_dev_name);
}

void ota_cancel(void)
{
    if (!g_active) {
        return;
    }

    k_timer_stop(&g_window_timer);
    g_active = false;

    /* Drop any pending disconnect re-advertise so it can't restart the
     * advertisement after the mesh resumes. */
    k_work_cancel_delayable(&g_readvertise_work);

    if (g_conn_adv != NULL) {
        int err = bt_le_ext_adv_stop(g_conn_adv);
        if (err) {
            LOG_WRN("OTA advertising stop failed: %d", err);
        }
    }

    /* Hand the controller back to the mesh. */
    ble_resume();
    LOG_INF("OTA window closed");
}

bool ota_is_active(void)
{
    return g_active;
}
