/*
 * Tissue Box — Networked sound trigger firmware
 *
 * Hardware: Adafruit Feather nRF52840 Express
 *           Adafruit Music Maker FeatherWing (VS1053B codec + SD card)
 *           SW-18010P vibration switch on A0 (P0.04)
 *
 * When vibration is detected, this device broadcasts a BLE extended
 * advertisement containing random sound/delay assignments for every
 * device in the network, then plays its own assigned sound after its
 * assigned delay.  When a matching BLE packet is received, the device
 * looks up its own assignment and plays accordingly.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_ficr.h>

#include "vs1053b.h"
#include "audio.h"
#include "ble.h"

LOG_MODULE_REGISTER(tissue_box, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                         */
/* ------------------------------------------------------------------ */

/*
 * KNOWN_DEVICE_IDS — lower byte of FICR->DEVICEID[0] for every device
 * in the network.  Must be identical across all devices.  Populate with
 * actual values read from each board (see README.md).
 */
static const uint8_t KNOWN_DEVICE_IDS[] = {
	0x00, 0x01, 0x02, 0x03, 0x04,
	0x05, 0x06, 0x07, 0x08, 0x09,
};
#define NUM_DEVICES ARRAY_SIZE(KNOWN_DEVICE_IDS)
BUILD_ASSERT(NUM_DEVICES <= 10, "Maximum 10 devices supported");

/* ------------------------------------------------------------------ */
/* Hardware references from devicetree                                */
/* ------------------------------------------------------------------ */

static const struct gpio_dt_spec vibration_sw =
	GPIO_DT_SPEC_GET(DT_NODELABEL(vibration_sw), gpios);

static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));

/* ------------------------------------------------------------------ */
/* Device identity                                                    */
/* ------------------------------------------------------------------ */

static uint8_t my_device_id;
static int     my_device_index;

static uint8_t ficr_device_id(void)
{
	return (uint8_t)(NRF_FICR->DEVICEID[0] & 0xFF);
}

static int find_device_index(uint8_t id)
{
	for (int i = 0; i < NUM_DEVICES; i++) {
		if (KNOWN_DEVICE_IDS[i] == id) {
			return i;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Vibration switch GPIO interrupt                                    */
/* ------------------------------------------------------------------ */

/*
 * Static initialization — the vibration handler thread starts at boot
 * (K_THREAD_DEFINE with delay=0, below) and immediately blocks on
 * this semaphore.  k_sem_init in main() would race with that.
 */
static K_SEM_DEFINE(vibration_sem, 0, 1);

static struct gpio_callback vibration_cb_data;

#define DEBOUNCE_MS 200
static int64_t last_trigger_time;

static void vibration_isr(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	int64_t now = k_uptime_get();
	if ((now - last_trigger_time) < DEBOUNCE_MS) {
		return;
	}
	last_trigger_time = now;
	k_sem_give(&vibration_sem);
}

static int gpio_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&vibration_sw)) {
		LOG_ERR("Vibration switch GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&vibration_sw, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure vibration switch: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&vibration_sw,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure vibration interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&vibration_cb_data, vibration_isr,
			   BIT(vibration_sw.pin));
	gpio_add_callback(vibration_sw.port, &vibration_cb_data);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Vibration handler thread                                           */
/* ------------------------------------------------------------------ */

/*
 * Set while this device is the trigger source.  Prevents the scan
 * callback from double-processing our own broadcast.  Cleared after
 * the triggering device finishes its own playback.
 *
 * Shared with the BLE module via pointer (see ble_init).
 */
static volatile bool am_triggering;
static volatile bool ble_ready;

static void vibration_handler(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sem_take(&vibration_sem, K_FOREVER);

		if (!ble_ready) {
			continue;
		}

		LOG_INF("Vibration detected — broadcasting assignments");

		am_triggering = true;

		int ret = ble_advertise_assignments();
		if (ret) {
			am_triggering = false;
			continue;
		}

		/* Retrieve our own assignment from the packet we just built */
		uint8_t  my_sound;
		uint16_t my_delay;
		ble_get_self_assignment(my_device_index, &my_sound, &my_delay);

		LOG_INF("Self assignment: sound=%u delay=%u ms",
			my_sound, my_delay);

		if (my_delay > 0) {
			k_msleep(my_delay);
		}

		/*
		 * Preempt any RX playback that was already in flight when
		 * am_triggering was set.  ble_cancel_rx_playback catches
		 * work that hasn't started yet; audio_cancel_playback
		 * causes an in-progress play_sound loop to exit early.
		 * Our own play call acquires the mutex, so it blocks until
		 * the RX playback has fully released it.
		 */
		ble_cancel_rx_playback();
		audio_cancel_playback();
		audio_play_sound(my_sound);

		am_triggering = false;
	}
}

#define VIBRATION_STACK_SIZE 4096
#define VIBRATION_PRIORITY   5

K_THREAD_DEFINE(vibration_tid, VIBRATION_STACK_SIZE,
		vibration_handler, NULL, NULL, NULL,
		VIBRATION_PRIORITY, 0, 0);

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
	LOG_INF("Tissue Box firmware starting");

	/* --- Device identity --- */
	my_device_id = ficr_device_id();
	my_device_index = find_device_index(my_device_id);

	LOG_INF("FICR Device ID: 0x%02x", my_device_id);

	if (my_device_index < 0) {
		LOG_ERR("Device ID 0x%02x NOT in KNOWN_DEVICE_IDS — halting",
			my_device_id);
		k_fatal_halt(K_ERR_KERNEL_OOPS);
		return -1;
	}
	LOG_INF("Device index: %d", my_device_index);

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
		LOG_WRN("SD card mount failed — playback disabled");
	}

	/* --- GPIO (vibration switch) --- */
	ret = gpio_init();
	if (ret) {
		return ret;
	}

	/* --- BLE --- */
	ret = ble_init(my_device_id, KNOWN_DEVICE_IDS, NUM_DEVICES,
		       &am_triggering);
	if (ret) {
		return ret;
	}
	ble_ready = true;

	LOG_INF("Tissue Box ready — waiting for vibration events");

	/*
	 * Returning from main lets the idle thread run, which executes
	 * the ARM WFE (Wait For Event) instruction.  The nRF52840 enters
	 * System ON idle — the lowest power state that keeps BLE scanning
	 * alive.  CPU draws ~1.5 uA in this state; radio dominates at
	 * ~5-7 mA.
	 */
	return 0;
}
