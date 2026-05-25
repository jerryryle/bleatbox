/*
 * VS1053B codec driver — SPI command/data interface.
 *
 * XCS and XDCS chip selects are managed by the Zephyr SPI driver via
 * embedded spi_cs_control structs, ensuring the bus lock is held for
 * the entire CS-active window.  Only DREQ (a pure input) is
 * configured manually.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include "vs1053b.h"

LOG_MODULE_REGISTER(vs1053b, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Registers and constants                                            */
/* ------------------------------------------------------------------ */

#define VS_WRITE_OP    0x02
#define VS_READ_OP     0x03

#define VS_REG_MODE    0x00
#define VS_REG_STATUS  0x01
#define VS_REG_CLOCKF  0x03
#define VS_REG_VOL     0x0B

#define VS_SM_RESET    BIT(2)
#define VS_SM_SDINEW   BIT(11)

/*
 * DREQ timeout: the VS1053B deasserts DREQ while its FIFO is full
 * and reasserts within ~1 ms under normal operation.  A 500 ms ceiling
 * catches wiring faults or power issues without false-triggering
 * during legitimate codec processing (e.g. codec startup, bitrate
 * changes).
 */
#define VS_DREQ_TIMEOUT_MS 500

/* ------------------------------------------------------------------ */
/* Hardware references from devicetree                                */
/* ------------------------------------------------------------------ */

#define ZUSER DT_PATH(zephyr_user)

static const struct gpio_dt_spec vs_dreq =
	GPIO_DT_SPEC_GET(ZUSER, vs1053b_dreq_gpios);

/*
 * VS1053B SPI configs with embedded chip-select control.
 *
 * The CS GPIO spec lives inside spi_config.cs (a struct value, not a
 * pointer — changed from older Zephyr APIs).  This ensures the SPI
 * driver holds the bus lock for the entire duration that CS is
 * asserted.  Without this, the SD card driver could interleave a
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

/* Cached SPI device pointer, set during init. */
static const struct device *spi;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * Wait for DREQ to go high (VS1053B ready for data/commands).
 *
 * Uses k_yield() rather than a DREQ-edge interrupt+semaphore because
 * DREQ toggles every 32-byte chunk during audio streaming (~0.3 ms at
 * 128 kbps).  The interrupt setup/teardown overhead per chunk would
 * exceed the polling cost.  k_yield() lets other threads run between
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
			LOG_ERR("DREQ timeout — check wiring/power");
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

	ret = spi_write(spi, &vs_cmd_spi_cfg, &tx_set);
	if (ret) {
		LOG_ERR("SPI write failed: %d", ret);
	}
	return ret;
}

static int vs_read_reg(uint8_t reg, uint16_t *val)
{
	/*
	 * Full-duplex transceive: send opcode + address + 2 dummy bytes,
	 * receive 2 garbage bytes + 2 data bytes simultaneously.  This is
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

	ret = spi_transceive(spi, &vs_cmd_spi_cfg, &tx_set, &rx_set);
	if (ret) {
		LOG_ERR("SPI read failed: %d", ret);
		return ret;
	}

	*val = ((uint16_t)rx[2] << 8) | rx[3];
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int vs1053b_write_data(const uint8_t *data, size_t len)
{
	struct spi_buf tx_buf = { .buf = (void *)data, .len = len };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };

	int ret = vs_wait_dreq();
	if (ret) return ret;

	ret = spi_write(spi, &vs_data_spi_cfg, &tx_set);
	if (ret) {
		LOG_ERR("SPI data write failed: %d", ret);
	}
	return ret;
}

int vs1053b_init(const struct device *spi_dev)
{
	spi = spi_dev;

	/*
	 * XCS and XDCS GPIO configuration is handled by the SPI driver
	 * via spi_cs_control — no manual setup needed.  Only DREQ
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
