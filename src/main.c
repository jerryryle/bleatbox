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
 * assigned delay. When a matching BLE packet is received, the device
 * looks up its own assignment and plays accordingly.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/random/random.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <hal/nrf_ficr.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(tissue_box, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                         */
/* ------------------------------------------------------------------ */

#define NUM_SOUNDS    10
#define DELAY_MIN_MS  0
#define DELAY_MAX_MS  2000

/*
 * KNOWN_DEVICE_IDS — lower byte of FICR->DEVICEID[0] for every device
 * in the network. Must be identical across all devices. Populate with
 * actual values read from each board (see README.md).
 */
static const uint8_t KNOWN_DEVICE_IDS[] = {
	0x00, 0x01, 0x02, 0x03, 0x04,
	0x05, 0x06, 0x07, 0x08, 0x09,
};
#define NUM_DEVICES ARRAY_SIZE(KNOWN_DEVICE_IDS)
BUILD_ASSERT(NUM_DEVICES <= 10, "Maximum 10 devices supported");

/* ------------------------------------------------------------------ */
/* BLE packet format                                                  */
/* ------------------------------------------------------------------ */

/* 0xFFFF = Bluetooth SIG reserved for testing; replace with a real   */
/* company ID if deployed publicly.                                   */
#define COMPANY_ID_LO  0xFF
#define COMPANY_ID_HI  0xFF
#define MAGIC_BYTE     0x42

#define ASSIGNMENT_ENTRY_SIZE  4   /* device_id(1) + sound(1) + delay(2) */
#define HEADER_SIZE            3   /* company_id(2) + magic(1) */
#define MFG_DATA_SIZE          (HEADER_SIZE + NUM_DEVICES * ASSIGNMENT_ENTRY_SIZE)

BUILD_ASSERT(MFG_DATA_SIZE <= 255,
	     "Manufacturer data exceeds extended advertisement limit");

/* ------------------------------------------------------------------ */
/* VS1053B registers and constants                                    */
/* ------------------------------------------------------------------ */

#define VS_WRITE_OP    0x02
#define VS_READ_OP     0x03

#define VS_REG_MODE    0x00
#define VS_REG_STATUS  0x01
#define VS_REG_CLOCKF  0x03
#define VS_REG_VOL     0x0B

#define VS_SM_RESET    BIT(2)
#define VS_SM_SDINEW   BIT(11)

#define VS_DATA_CHUNK  32

/*
 * DREQ timeout: the VS1053B deasserts DREQ while its FIFO is full
 * and reasserts within ~1 ms under normal operation. A 500 ms ceiling
 * catches wiring faults or power issues without false-triggering
 * during legitimate codec processing (e.g. codec startup, bitrate
 * changes).
 */
#define VS_DREQ_TIMEOUT_MS 500

/* ------------------------------------------------------------------ */
/* Hardware references from devicetree                                */
/* ------------------------------------------------------------------ */

/* Vibration switch: gpio-keys node in the overlay. */
static const struct gpio_dt_spec vibration_sw =
	GPIO_DT_SPEC_GET(DT_NODELABEL(vibration_sw), gpios);

/*
 * VS1053B DREQ: polled input from the zephyr,user node.
 * XCS and XDCS are NOT separate gpio_dt_specs — they live inside the
 * spi_cs_control structs below so the SPI driver manages them
 * atomically with bus locking.
 */
#define ZUSER DT_PATH(zephyr_user)

static const struct gpio_dt_spec vs_dreq =
	GPIO_DT_SPEC_GET(ZUSER, vs1053b_dreq_gpios);

static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));

/*
 * VS1053B SPI configs with embedded chip-select control.
 *
 * The CS GPIO spec lives inside spi_config.cs (a struct value, not a
 * pointer — changed from older Zephyr APIs). This ensures the SPI
 * driver holds the bus lock for the entire duration that CS is
 * asserted. Without this, the SD card driver could interleave a
 * transaction while the VS1053B's CS was low.
 *
 * Mode 0 (CPOL=0, CPHA=0), MSB first.
 * Command clock <= CLKI/7 ~ 1.8 MHz at default 12.288 MHz CLKI.
 * 2 MHz is safe before and after the SCI_CLOCKF boost.
 */
static const struct spi_config vs_cmd_spi_cfg = {
	.frequency = 2000000,
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.cs = {
		.gpio = GPIO_DT_SPEC_GET(ZUSER, vs1053b_xcs_gpios),
		.delay = 0,
		.cs_is_gpio = true,
	},
};

/* Data interface can run at CLKI/4 after clock boost. */
static const struct spi_config vs_data_spi_cfg = {
	.frequency = 6000000,
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	.cs = {
		.gpio = GPIO_DT_SPEC_GET(ZUSER, vs1053b_xdcs_gpios),
		.delay = 0,
		.cs_is_gpio = true,
	},
};

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static uint8_t my_device_id;
static int     my_device_index;

static struct gpio_callback vibration_cb_data;

/*
 * Static initialization — the vibration handler thread starts at boot
 * (K_THREAD_DEFINE with delay=0, below) and immediately blocks on
 * this semaphore. k_sem_init in main() would race with that.
 */
static K_SEM_DEFINE(vibration_sem, 0, 1);

static struct bt_le_ext_adv *adv_set;
static volatile bool         ble_ready;

/*
 * Set while this device is the trigger source. Prevents the scan
 * callback from double-processing our own broadcast. Cleared after
 * the triggering device finishes its own playback.
 */
static volatile bool am_triggering;

#define DEBOUNCE_MS 200
static int64_t last_trigger_time;

/* Manufacturer data buffer shared between build and advertise. */
static uint8_t mfg_data[MFG_DATA_SIZE];

/* ------------------------------------------------------------------ */
/* RX playback work queue — offloads sound playback from BLE thread   */
/* ------------------------------------------------------------------ */

static K_THREAD_STACK_DEFINE(rx_wq_stack, 4096);
static struct k_work_q rx_work_q;
static struct k_work_delayable rx_playback_work;
static uint8_t  rx_sound_index;

/*
 * Playback mutex: prevents the vibration handler thread and the RX
 * work queue from streaming interleaved audio chunks to the VS1053B.
 * The SPI driver serializes individual 32-byte transactions, but the
 * VS1053B interprets the byte stream as a continuous audio file — if
 * two threads alternate chunks from different MP3 files, the codec
 * sees a corrupt bitstream and produces noise or silence.
 */
static K_MUTEX_DEFINE(playback_mutex);

/*
 * Set by the vibration handler to preempt an in-progress RX playback.
 * The play_sound loop checks this between chunks and exits early,
 * allowing the vibration handler to acquire the mutex without waiting
 * for the entire RX track to finish.
 */
static volatile bool playback_cancel;

static void rx_playback_handler(struct k_work *work);
static void play_sound(uint8_t sound_index);

/* ------------------------------------------------------------------ */
/* Device identity                                                    */
/* ------------------------------------------------------------------ */

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
/* VS1053B driver (SPI, no bit-bang)                                  */
/* ------------------------------------------------------------------ */

/*
 * Wait for DREQ to go high (VS1053B ready for data/commands).
 *
 * Uses k_yield() rather than a DREQ-edge interrupt+semaphore because
 * DREQ toggles every 32-byte chunk during audio streaming (~0.3 ms at
 * 128 kbps). The interrupt setup/teardown overhead per chunk would
 * exceed the polling cost. k_yield() lets other threads run between
 * checks while keeping wake-up latency to one scheduler tick.
 *
 * Returns 0 on success, -ETIMEDOUT if DREQ stayed low for
 * VS_DREQ_TIMEOUT_MS (likely a hardware fault).
 */
static int vs_wait_dreq(void)
{
	int64_t deadline = k_uptime_get() + VS_DREQ_TIMEOUT_MS;

	while (gpio_pin_get_dt(&vs_dreq) == 0) {
		if (k_uptime_get() >= deadline) {
			LOG_ERR("VS1053B DREQ timeout — check wiring/power");
			return -ETIMEDOUT;
		}
		k_yield();
	}
	return 0;
}

static int vs_write_reg(uint8_t reg, uint16_t val)
{
	uint8_t tx[4] = {
		VS_WRITE_OP,
		reg,
		(uint8_t)(val >> 8),
		(uint8_t)(val & 0xFF),
	};

	struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };

	int ret = vs_wait_dreq();
	if (ret) return ret;

	ret = spi_write(spi_dev, &vs_cmd_spi_cfg, &tx_set);
	if (ret) {
		LOG_ERR("VS1053B SPI write failed: %d", ret);
	}
	return ret;
}

static int vs_read_reg(uint8_t reg, uint16_t *val)
{
	/*
	 * Full-duplex transceive: send opcode + address + 2 dummy bytes,
	 * receive 2 garbage bytes + 2 data bytes simultaneously. This is
	 * one atomic SPI transaction — the driver holds XCS low and the
	 * bus lock for the entire 4-byte exchange.
	 */
	uint8_t tx[4] = { VS_READ_OP, reg, 0x00, 0x00 };
	uint8_t rx[4] = { 0 };

	struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
	struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	int ret = vs_wait_dreq();
	if (ret) return ret;

	ret = spi_transceive(spi_dev, &vs_cmd_spi_cfg, &tx_set, &rx_set);
	if (ret) {
		LOG_ERR("VS1053B SPI read failed: %d", ret);
		return ret;
	}

	*val = ((uint16_t)rx[2] << 8) | rx[3];
	return 0;
}

static int vs_write_data(const uint8_t *data, size_t len)
{
	struct spi_buf tx_buf = { .buf = (void *)data, .len = len };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };

	int ret = vs_wait_dreq();
	if (ret) return ret;

	ret = spi_write(spi_dev, &vs_data_spi_cfg, &tx_set);
	if (ret) {
		LOG_ERR("VS1053B SPI data write failed: %d", ret);
	}
	return ret;
}

static int vs1053b_init(void)
{
	/*
	 * XCS and XDCS GPIO configuration is handled by the SPI driver
	 * via spi_cs_control — no manual setup needed. Only DREQ
	 * (a pure input, not tied to SPI) requires explicit configuration.
	 */
	int ret = gpio_pin_configure_dt(&vs_dreq, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure DREQ: %d", ret);
		return ret;
	}

	ret = vs_write_reg(VS_REG_MODE, VS_SM_SDINEW | VS_SM_RESET);
	if (ret) return ret;
	k_msleep(5);
	ret = vs_wait_dreq();
	if (ret) return ret;

	/*
	 * SCI_CLOCKF = 0x8800: XTALI x 3.5 = 43 MHz internal clock.
	 * Allows faster SPI data transfers and reduces codec latency.
	 */
	ret = vs_write_reg(VS_REG_CLOCKF, 0x8800);
	if (ret) return ret;
	k_msleep(2);

	/* 0x1010: moderate volume, both channels. 0x00=max, 0xFE=silence. */
	ret = vs_write_reg(VS_REG_VOL, 0x1010);
	if (ret) return ret;

	uint16_t status;
	ret = vs_read_reg(VS_REG_STATUS, &status);
	if (ret) return ret;

	LOG_INF("VS1053B status: 0x%04x", status);
	return 0;
}

/* ------------------------------------------------------------------ */
/* SD card and filesystem                                             */
/* ------------------------------------------------------------------ */

static FATFS fat_fs;
static struct fs_mount_t mount_point = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};
static bool sd_mounted;

static int sd_mount(void)
{
	static const char *disk = "SD";
	uint32_t block_count, block_size;

	if (disk_access_init(disk) != 0) {
		LOG_ERR("SD card init failed");
		return -EIO;
	}
	disk_access_ioctl(disk, DISK_IOCTL_GET_SECTOR_COUNT, &block_count);
	disk_access_ioctl(disk, DISK_IOCTL_GET_SECTOR_SIZE, &block_size);
	LOG_INF("SD card: %u sectors, %u bytes/sector", block_count, block_size);

	mount_point.storage_dev = (void *)disk;
	int ret = fs_mount(&mount_point);
	if (ret != 0) {
		LOG_ERR("FAT mount failed: %d", ret);
		return ret;
	}

	sd_mounted = true;
	LOG_INF("SD card mounted at %s", mount_point.mnt_point);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Sound playback                                                     */
/* ------------------------------------------------------------------ */

static void play_sound(uint8_t sound_index)
{
	k_mutex_lock(&playback_mutex, K_FOREVER);
	playback_cancel = false;

	if (!sd_mounted) {
		LOG_ERR("SD not mounted, cannot play sound %u", sound_index);
		k_mutex_unlock(&playback_mutex);
		return;
	}

	char path[32];
	snprintf(path, sizeof(path), "/SD:/%u.mp3", sound_index);

	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("Cannot open %s: %d", path, ret);
		k_mutex_unlock(&playback_mutex);
		return;
	}

	LOG_INF("Playing %s", path);

	uint8_t buf[VS_DATA_CHUNK];
	ssize_t nread;
	while ((nread = fs_read(&f, buf, sizeof(buf))) > 0) {
		if (playback_cancel) {
			LOG_INF("Playback preempted");
			break;
		}
		ret = vs_write_data(buf, nread);
		if (ret) {
			LOG_ERR("Playback aborted: SPI error");
			break;
		}
	}

	fs_close(&f);
	k_mutex_unlock(&playback_mutex);
	LOG_INF("Playback finished");
}

/* ------------------------------------------------------------------ */
/* RX playback — runs on dedicated work queue (not BLE thread)        */
/* ------------------------------------------------------------------ */

static void rx_playback_handler(struct k_work *work)
{
	play_sound(rx_sound_index);
}

/* ------------------------------------------------------------------ */
/* BLE — Extended advertising (transmit)                              */
/* ------------------------------------------------------------------ */

static void build_mfg_data(void)
{
	mfg_data[0] = COMPANY_ID_LO;
	mfg_data[1] = COMPANY_ID_HI;
	mfg_data[2] = MAGIC_BYTE;

	for (int i = 0; i < NUM_DEVICES; i++) {
		int offset = HEADER_SIZE + i * ASSIGNMENT_ENTRY_SIZE;

		uint8_t  sound = (uint8_t)(sys_rand32_get() % NUM_SOUNDS);
		uint16_t delay = DELAY_MIN_MS +
			(uint16_t)(sys_rand32_get() %
				   (DELAY_MAX_MS - DELAY_MIN_MS + 1));

		mfg_data[offset + 0] = KNOWN_DEVICE_IDS[i];
		mfg_data[offset + 1] = sound;
		sys_put_le16(delay, &mfg_data[offset + 2]);
	}
}

static int ble_advertise_assignments(void)
{
	build_mfg_data();

	struct bt_data ad[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
	};

	int ret = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad),
					 NULL, 0);
	if (ret) {
		LOG_ERR("Failed to set adv data: %d", ret);
		return ret;
	}

	/*
	 * Send 5 advertising events. With the 20-30 ms advertising
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

/* ------------------------------------------------------------------ */
/* BLE — Extended scanning (receive)                                  */
/* ------------------------------------------------------------------ */

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
			 struct net_buf_simple *buf)
{
	/*
	 * Ignore broadcasts while we are the trigger source. Our own
	 * assignment is handled directly in the vibration thread, so
	 * processing our own reflected advertisement would double-play.
	 */
	if (am_triggering) {
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
		    data_len != MFG_DATA_SIZE) {
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
		for (int i = 0; i < NUM_DEVICES; i++) {
			int off = HEADER_SIZE + i * ASSIGNMENT_ENTRY_SIZE;
			if (data[off] != my_device_id) {
				continue;
			}

			uint8_t  sound = data[off + 1];
			uint16_t delay = sys_get_le16(&data[off + 2]);

			LOG_INF("RX assignment: sound=%u delay=%u ms",
				sound, delay);

			/*
			 * Cancel any pending playback from a prior broadcast
			 * before overwriting rx_sound_index. Without this,
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

		LOG_WRN("Device ID 0x%02x not in broadcast", my_device_id);
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
 * Cost: ~5-7 mA continuous radio current. A duty-cycled alternative
 * (e.g. window=30ms / interval=50ms, 60% duty) would save ~40% radio
 * power but risk 20 ms worst-case detection latency, eating into the
 * 50 ms budget. Since vibration events are rare and the CPU still
 * enters System ON idle between scan events, continuous scanning is
 * the right tradeoff here.
 *
 * 48 units x 0.625 ms = 30 ms, matching the minimum extended
 * advertising interval. Window == interval == 30 ms = continuous.
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
/* Vibration switch GPIO interrupt                                    */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Vibration handler thread                                           */
/* ------------------------------------------------------------------ */

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

		/* Play our own assignment from the packet we just built */
		int off = HEADER_SIZE + my_device_index * ASSIGNMENT_ENTRY_SIZE;
		uint8_t  my_sound = mfg_data[off + 1];
		uint16_t my_delay = sys_get_le16(&mfg_data[off + 2]);

		LOG_INF("Self assignment: sound=%u delay=%u ms",
			my_sound, my_delay);

		if (my_delay > 0) {
			k_msleep(my_delay);
		}

		/*
		 * Preempt any RX playback that was already in flight when
		 * am_triggering was set. playback_cancel causes the RX
		 * play_sound loop to exit early; k_work_cancel catches
		 * work that hasn't started yet. Our own play_sound call
		 * acquires the mutex, so it blocks until the RX play_sound
		 * has fully released it.
		 */
		playback_cancel = true;
		k_work_cancel_delayable(&rx_playback_work);

		play_sound(my_sound);

		am_triggering = false;
	}
}

#define VIBRATION_STACK_SIZE 4096
#define VIBRATION_PRIORITY   5

K_THREAD_DEFINE(vibration_tid, VIBRATION_STACK_SIZE,
		vibration_handler, NULL, NULL, NULL,
		VIBRATION_PRIORITY, 0, 0);

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

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

static int ble_init(void)
{
	int ret;

	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return ret;
	}
	LOG_INF("Bluetooth initialized");

	/*
	 * Extended advertising at 20-30 ms intervals. The previous
	 * BT_GAP_ADV_FAST_INT_MIN_2 (100 ms) was too slow — with a
	 * 100 ms timeout only one event would ever fire. At 20-30 ms,
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

	ble_ready = true;
	return 0;
}

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

	/* --- RX playback work queue with dedicated stack --- */
	k_work_queue_init(&rx_work_q);
	k_work_queue_start(&rx_work_q, rx_wq_stack,
			   K_THREAD_STACK_SIZEOF(rx_wq_stack),
			   VIBRATION_PRIORITY, NULL);
	k_work_init_delayable(&rx_playback_work, rx_playback_handler);

	/* --- SPI --- */
	if (!device_is_ready(spi_dev)) {
		LOG_ERR("SPI device not ready");
		return -ENODEV;
	}

	/* --- VS1053B codec --- */
	int ret = vs1053b_init();
	if (ret) {
		LOG_ERR("VS1053B init failed: %d", ret);
		return ret;
	}
	LOG_INF("VS1053B initialized");

	/* --- SD card --- */
	ret = sd_mount();
	if (ret) {
		LOG_WRN("SD card mount failed — playback disabled");
	}

	/* --- GPIO (vibration switch) --- */
	ret = gpio_init();
	if (ret) {
		return ret;
	}

	/* --- BLE --- */
	ret = ble_init();
	if (ret) {
		return ret;
	}

	LOG_INF("Tissue Box ready — waiting for vibration events");

	/*
	 * Returning from main lets the idle thread run, which executes
	 * the ARM WFE (Wait For Event) instruction. The nRF52840 enters
	 * System ON idle — the lowest power state that keeps BLE scanning
	 * alive. CPU draws ~1.5 uA in this state; radio dominates at
	 * ~5-7 mA.
	 */
	return 0;
}
