/*
 * BleatBox — Networked sound trigger firmware
 *
 * Hardware: Adafruit Feather nRF52840 Express
 *           Adafruit Music Maker FeatherWing (VS1053B codec + SD card)
 *           LIS2DW12 accelerometer on I2C0, INT1 on A0 (P0.04)
 *
 * Architecture: event-driven main loop.
 *
 * The vibration and BLE modules are pure event producers — they push
 * events into a shared k_msgq with no knowledge of each other or the
 * audio subsystem.  The main loop is the sole consumer: it waits for
 * events, decides whether to act, and orchestrates BLE broadcasting
 * and sound playback.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "compose.h"
#include "device_config.h"
#include "events.h"
#include "accel.h"
#include "battery.h"
#include "sdcard.h"
#include "sounds.h"
#include "audio.h"
#include "ble.h"

LOG_MODULE_REGISTER(bleatbox, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Onboard LED (boot indicator)                                       */
/* ------------------------------------------------------------------ */

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec g_led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                         */
/* ------------------------------------------------------------------ */

/* Sound played locally when this device detects vibration. */
#define VIBRATION_SOUND_INDEX 0

/* ------------------------------------------------------------------ */
/* Event queue                                                        */
/* ------------------------------------------------------------------ */

/*
 * Depth 4: enough to buffer a burst of events while the main loop
 * is handling one.  Excess events are silently dropped at the
 * producer (k_msgq_put with K_NO_WAIT), which is the desired
 * behavior — we never queue up a backlog of sounds.
 */
#define EVENT_QUEUE_DEPTH 4
K_MSGQ_DEFINE(event_q, sizeof(struct event), EVENT_QUEUE_DEPTH, 4);

/* ------------------------------------------------------------------ */
/* Vibration cooldown                                                 */
/* ------------------------------------------------------------------ */

#define VIBRATION_COOLDOWN_MS 1000

static volatile bool g_drop_vibration_events;

static void cooldown_expired(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    g_drop_vibration_events = false;
    LOG_INF("Vibration cooldown expired");
}

static K_TIMER_DEFINE(g_vibration_cooldown_timer, cooldown_expired, NULL);

/* ------------------------------------------------------------------ */
/* Event handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_vibration(void)
{
    /*
     * A playing sound (e.g. a BLE-assigned one) shakes the enclosure;
     * don't broadcast assignments for vibration we likely caused
     * ourselves while our own playback request would be discarded.
     * This also covers the window between a claim and the main loop
     * processing its EVENT_AUDIO_START.
     */
    if (audio_is_playing()) {
        LOG_INF("Vibration dropped — sound already playing");
        return;
    }

    LOG_INF("Vibration detected");

    char path[32];
    int ret = sounds_get_path(SOUND_TYPE_GOAT, VIBRATION_SOUND_INDEX,
                              path, sizeof(path));
    if (ret) {
        LOG_ERR("Cannot resolve sound %u: %d", VIBRATION_SOUND_INDEX, ret);
        return;
    }

    if (audio_play_sound(path, 0)) {
        /* Lost a race with another codec claim — don't broadcast a
         * message for a sound we didn't play. */
        return;
    }

    uint8_t payload[16];
    int err = compose_message(payload);
    if (err) {
        LOG_WRN("Message compose failed (%d) — check config and sound count",
                err);
        return;
    }

    LOG_INF("Broadcasting bleat message");
    ble_broadcast_message(payload);
}

static void handle_ble_rx(const struct event *evt)
{
    if (audio_is_playing()) {
        LOG_INF("BLE RX dropped — sound already playing");
        return;
    }

    enum sound_type type = ble_sound_decode_type(evt->sound);
    uint8_t index = ble_sound_decode_index(evt->sound);

    char path[32];
    int ret = sounds_get_path(type, index, path, sizeof(path));
    if (ret) {
        LOG_WRN("BLE RX dropped — invalid sound 0x%02x", evt->sound);
        return;
    }

    LOG_INF("BLE RX assignment: sound=0x%02x delay=%u ms",
            evt->sound, evt->delay_ms);

    audio_play_sound(path, evt->delay_ms);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    LOG_INF("BleatBox firmware starting");

    /* --- Boot indicator (blink LED 3 times) --- */
    if (gpio_is_ready_dt(&g_led)) {
        gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
        for (int i = 0; i < 3; i++) {
            gpio_pin_set_dt(&g_led, 1);
            k_msleep(100);
            gpio_pin_set_dt(&g_led, 0);
            k_msleep(100);
        }
    }

    if (audio_init(&event_q)) {
        LOG_ERR("Audio init failed — playback disabled");
    }
    sdcard_init();
    sounds_init();
    if (accel_init(&event_q)) {
        LOG_ERR("Accelerometer init failed — vibration triggers disabled");
    }
    if (battery_init()) {
        LOG_ERR("Battery monitor init failed — battery reads disabled");
    }
    if (ble_init(&event_q)) {
        LOG_ERR("BLE init failed — broadcasts and relays disabled");
    }

    struct device_config cfg;
    device_config_defaults(&cfg);

    if (sdcard_mount()) {
        LOG_ERR("SD card mount failed — playback disabled, using default config");
    } else {
        if (sounds_scan()) {
            LOG_ERR("Sound scan failed — playback disabled");
        }
        if (device_config_load(&cfg)) {
            LOG_ERR("Device config load failed — using default config");
            device_config_defaults(&cfg);
        }
    }

    /* --- Volume from config --- */
    audio_set_volume(cfg.volume);

    /* --- Message composition config --- */
    compose_init(cfg.delay_min_ms, cfg.delay_max_ms,
                 VIBRATION_SOUND_INDEX + 1,
                 sounds_get_count(SOUND_TYPE_GOAT));

    /* --- Start producers (any-motion interrupt, BLE scan) --- */
    if (accel_start(cfg.accel_threshold_mg)) {
        LOG_ERR("Accelerometer start failed — vibration triggers disabled");
    }
    if (ble_start(cfg.id, cfg.relay_ttl)) {
        LOG_ERR("BLE start failed — broadcasts and relays disabled");
    }

    g_drop_vibration_events = false;

    LOG_INF("BleatBox ready — waiting for events");

    /* --- Event loop --- */
    struct event evt;
    for (;;) {
        /*
         * Block until an event arrives.  While blocked, the idle
         * thread runs and the nRF52840 enters System ON idle (ARM
         * WFE) — the lowest power state that keeps BLE scanning
         * alive.
         */
        k_msgq_get(&event_q, &evt, K_FOREVER);

        switch (evt.type) {
        case EVENT_VIBRATION:
            if (g_drop_vibration_events) {
                LOG_INF("Vibration dropped — cooldown active");
            } else {
                handle_vibration();
            }
            break;
        case EVENT_BLE_RX:
            handle_ble_rx(&evt);
            break;
        case EVENT_AUDIO_START:
            /*
             * Suppress vibration triggers for the whole audio
             * session. Stop any pending cooldown timer so a stale
             * expiry from a previous playback can't clear the flag
             * while this one is still running.
             */
            k_timer_stop(&g_vibration_cooldown_timer);
            g_drop_vibration_events = true;
            break;
        case EVENT_AUDIO_DONE:
            /* Keep suppressing until the enclosure rings down. */
            k_timer_start(&g_vibration_cooldown_timer,
                          K_MSEC(VIBRATION_COOLDOWN_MS), K_NO_WAIT);
            break;
        }
    }

    return 0; /* unreachable */
}
