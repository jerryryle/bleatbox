/*
 * BleatBox — Networked sound trigger firmware
 *
 * Hardware: Adafruit Feather nRF52840 Express
 *           Adafruit Music Maker FeatherWing (VS1053B codec + SD card)
 *           SW-18010P vibration switch on A0 (P0.04)
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
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include "assignments.h"
#include "device_config.h"
#include "events.h"
#include "vibration.h"
#include "vs1053b.h"
#include "audio.h"
#include "ble.h"

LOG_MODULE_REGISTER(bleatbox, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                         */
/* ------------------------------------------------------------------ */

/* Sound played locally when this device detects vibration. */
#define VIBRATION_SOUND_INDEX 0

/* ------------------------------------------------------------------ */
/* Hardware references from devicetree                                */
/* ------------------------------------------------------------------ */

static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));

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
/* Event handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_vibration(void)
{
	if (audio_is_playing()) {
		LOG_INF("Vibration dropped — sound already playing");
		return;
	}

	uint8_t num_peers;
	const uint8_t *peer_ids = device_config_get_peers(&num_peers);

	LOG_INF("Vibration detected — playing sound %u, broadcasting assignments",
		VIBRATION_SOUND_INDEX);

	struct ble_assignment assignments[DEVICE_CONFIG_MAX_PEERS];
	assignments_generate(peer_ids, num_peers, assignments);

	audio_play_sound(VIBRATION_SOUND_INDEX);
	ble_advertise_assignments(assignments, num_peers);
}

static void handle_ble_rx(const struct event *evt)
{
	if (audio_is_playing()) {
		LOG_INF("BLE RX dropped — sound already playing");
		return;
	}

	LOG_INF("BLE RX assignment: sound=%u delay=%u ms",
		evt->sound, evt->delay_ms);

	if (evt->delay_ms > 0) {
		k_msleep(evt->delay_ms);
	}

	audio_play_sound(evt->sound);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
	LOG_INF("BleatBox firmware starting");

	/* --- SPI --- */
	if (!device_is_ready(spi_dev)) {
		LOG_ERR("SPI device not ready");
		return -ENODEV;
	}

	/* --- VS1053B codec --- */
	int ret = vs1053b_init(spi_dev);
	if (ret) {
		LOG_ERR("VS1053B init failed: %d", ret);
		return ret;
	}
	LOG_INF("VS1053B initialized");

	/* --- SD card --- */
	ret = audio_sd_mount();
	if (ret) {
		LOG_ERR("SD card mount failed — shell still available over USB");
		k_sleep(K_FOREVER);
		return 0;
	}

	/* --- Device configuration --- */
	ret = device_config_load();
	if (ret) {
		LOG_ERR("Device config load failed — shell still available over USB");
		k_sleep(K_FOREVER);
		return 0;
	}
	uint8_t my_device_id = device_config_get_id();
	LOG_INF("Device ID: 0x%02x", my_device_id);

	/* --- Vibration switch --- */
	ret = vibration_init(&event_q);
	if (ret) {
		return ret;
	}

	/* --- BLE --- */
	ret = ble_init(my_device_id, &event_q);
	if (ret) {
		return ret;
	}

	LOG_INF("BleatBox ready — waiting for events");

	/* --- Event loop --- */
	struct event evt;
	while (1) {
		/*
		 * Block until an event arrives.  While blocked, the idle
		 * thread runs and the nRF52840 enters System ON idle (ARM
		 * WFE) — the lowest power state that keeps BLE scanning
		 * alive.
		 */
		k_msgq_get(&event_q, &evt, K_FOREVER);

		switch (evt.type) {
		case EVENT_VIBRATION:
			handle_vibration();
			break;
		case EVENT_BLE_RX:
			handle_ble_rx(&evt);
			break;
		}
	}

	return 0; /* unreachable */
}
