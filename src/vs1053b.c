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

#define VS_WRITE_OP         0x02
#define VS_READ_OP          0x03

#define VS_REG_MODE         0x00
#define VS_REG_STATUS       0x01
#define VS_REG_BASS         0x02
#define VS_REG_CLOCKF       0x03
#define VS_REG_DECODE_TIME  0x04
#define VS_REG_AUDATA       0x05
#define VS_REG_WRAM         0x06
#define VS_REG_WRAMADDR     0x07
#define VS_REG_HDAT0        0x08
#define VS_REG_HDAT1        0x09
#define VS_REG_AIADDR       0x0A
#define VS_REG_VOL          0x0B
#define VS_REG_AICTRL0      0x0C
#define VS_REG_AICTRL1      0x0D
#define VS_REG_AICTRL2      0x0E
#define VS_REG_AICTRL3      0x0F

#define VS_MODE_RESET       BIT(2)
#define VS_MODE_CANCEL      BIT(3)
#define VS_MODE_TESTS       BIT(5)
#define VS_MODE_SDINEW      BIT(11)

#define VS_STATUS_APDOWN1   BIT(2)
#define VS_STATUS_APDOWN2   BIT(3)

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

static const struct gpio_dt_spec g_vs_dreq =
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
static const struct spi_config g_vs_cmd_spi_cfg = {
    .frequency = 2000000,
    .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .cs = {
        .gpio = GPIO_DT_SPEC_GET(ZUSER, vs1053b_xcs_gpios),
        .delay = 0,
        .cs_is_gpio = true,
    },
};

/* Data interface can run at CLKI/4 after clock boost. */
static const struct spi_config g_vs_data_spi_cfg = {
    .frequency = 6000000,
    .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .cs = {
        .gpio = GPIO_DT_SPEC_GET(ZUSER, vs1053b_xdcs_gpios),
        .delay = 0,
        .cs_is_gpio = true,
    },
};

/* Cached SPI device pointer, set during init. */
static const struct device *g_spi;

/* Last volume set via vs1053b_set_volume(), restored on power-up. */
static uint16_t g_vol_reg;


/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * Wait for DREQ to go high (VS1053B ready for data/commands).
 *
 * Sleeps 1 ms between polls so lower-priority threads (shell, logging)
 * can run during audio streaming.  DREQ reasserts in ~0.3 ms at typical
 * bitrates; the codec's 2048-byte FIFO absorbs the extra latency.
 *
 * Returns 0 on success, -ETIMEDOUT if DREQ stayed low for
 * VS_DREQ_TIMEOUT_MS (likely a hardware fault).
 */
static int vs_wait_dreq(void)
{
    int64_t deadline = k_uptime_get() + VS_DREQ_TIMEOUT_MS;

    while (gpio_pin_get_dt(&g_vs_dreq) == 0) {
        if (k_uptime_get() >= deadline) {
            LOG_ERR("DREQ timeout — check wiring/power");
            return -ETIMEDOUT;
        }
        k_msleep(1);
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

    struct spi_buf tx_buf = {.buf = tx, .len = sizeof(tx)};
    struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};

    int ret = vs_wait_dreq();
    if (ret)
        return ret;

    ret = spi_write(g_spi, &g_vs_cmd_spi_cfg, &tx_set);
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
    uint8_t tx[4] = {VS_READ_OP, reg, 0x00, 0x00};
    uint8_t rx[4] = {0};

    struct spi_buf tx_buf = {.buf = tx, .len = sizeof(tx)};
    struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
    struct spi_buf rx_buf = {.buf = rx, .len = sizeof(rx)};
    struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

    int ret = vs_wait_dreq();
    if (ret)
        return ret;

    ret = spi_transceive(g_spi, &g_vs_cmd_spi_cfg, &tx_set, &rx_set);
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

int vs1053b_get_volume(uint8_t *percent)
{
    uint16_t reg;
    int ret = vs_read_reg(VS_REG_VOL, &reg);
    if (ret) {
        return ret;
    }

    /* Use left channel (high byte). 0x00 = max, 0xFE = silence. */
    unsigned int att = reg >> 8;
    if (att >= 254) {
        *percent = 0;
    } else {
        *percent = (uint8_t)((254 - att) * 100 / 254);
    }
    return 0;
}

int vs1053b_set_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    /* 0x00 = max, 0xFE = silence.  Map 100 → 0x00, 0 → 0xFE. */
    uint8_t att = (uint8_t)((uint16_t)(100 - percent) * 254 / 100);
    g_vol_reg = ((uint16_t)att << 8) | att;

    return vs_write_reg(VS_REG_VOL, g_vol_reg);
}

int vs1053b_write_data(const uint8_t *data, size_t len)
{
    struct spi_buf tx_buf = {.buf = (void *)data, .len = len};
    struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};

    int ret = vs_wait_dreq();
    if (ret)
        return ret;

    ret = spi_write(g_spi, &g_vs_data_spi_cfg, &tx_set);
    if (ret) {
        LOG_ERR("SPI data write failed: %d", ret);
    }
    return ret;
}

int vs1053b_write_reg(uint8_t reg, uint16_t val)
{
    return vs_write_reg(reg, val);
}

int vs1053b_sine_test(bool enable)
{
    int ret;

    if (enable) {
        ret = vs_write_reg(VS_REG_MODE, VS_MODE_SDINEW | VS_MODE_TESTS);
        if (ret)
            return ret;
        k_msleep(1);

        /* 0x44 = ~4.4 kHz sine at 12.288 MHz XTALI */
        uint8_t start[] = {
            0x53,
            0xEF,
            0x6E,
            0x44,
            0x00,
            0x00,
            0x00,
            0x00,
        };
        ret = vs1053b_write_data(start, sizeof(start));
    } else {
        uint8_t stop[] = {
            0x45,
            0x78,
            0x69,
            0x74,
            0x00,
            0x00,
            0x00,
            0x00,
        };
        ret = vs1053b_write_data(stop, sizeof(stop));
        if (ret)
            return ret;
        k_msleep(1);

        ret = vs_write_reg(VS_REG_MODE, VS_MODE_SDINEW);
    }
    return ret;
}

int vs1053b_end_playback(void)
{
    uint16_t mode;
    int ret = vs_read_reg(VS_REG_MODE, &mode);
    if (ret) {
        return ret;
    }

    ret = vs_write_reg(VS_REG_MODE, mode | VS_MODE_CANCEL);
    if (ret) {
        return ret;
    }

    /* Send 32 bytes of zeros (endFillByte is 0 for most codecs)
     * repeatedly until SM_CANCEL clears, meaning the codec has
     * finished flushing its internal buffers. */
    uint8_t zeros[VS1053B_DATA_CHUNK] = {0};
    for (int i = 0; i < 64; i++) {
        ret = vs1053b_write_data(zeros, sizeof(zeros));
        if (ret) {
            break;
        }

        ret = vs_read_reg(VS_REG_MODE, &mode);
        if (ret) {
            break;
        }
        if (!(mode & VS_MODE_CANCEL)) {
            return 0;
        }
    }

    /* SM_CANCEL didn't clear — do a hard soft-reset as fallback */
    ret = vs_write_reg(VS_REG_MODE, VS_MODE_SDINEW | VS_MODE_RESET);
    if (ret) {
        return ret;
    }
    k_msleep(5);
    return vs_wait_dreq();
}

int vs1053b_power_down(void)
{
    /* Silence analog outputs to avoid pop */
    int ret = vs_write_reg(VS_REG_VOL, 0xFFFF);
    if (ret) {
        return ret;
    }

    /* Lower sample rate to reduce audio interrupt overhead */
    ret = vs_write_reg(VS_REG_AUDATA, 0x0010);
    if (ret) {
        return ret;
    }

    /* Disable PLL — drop to 1.0× crystal clock */
    ret = vs_write_reg(VS_REG_CLOCKF, 0x0000);
    if (ret) {
        return ret;
    }

    LOG_INF("VS1053B low-power mode entered");
    return 0;
}

int vs1053b_power_up(void)
{
    /* Restore PLL (3.5× crystal = 43 MHz internal clock) */
    int ret = vs_write_reg(VS_REG_CLOCKF, 0x8800);
    if (ret) {
        return ret;
    }
    k_msleep(2);

    /* Restore volume */
    ret = vs_write_reg(VS_REG_VOL, g_vol_reg);
    if (ret) {
        return ret;
    }

    LOG_INF("VS1053B woke from low-power mode");
    return 0;
}

int vs1053b_init(const struct device *spi_dev)
{
    g_spi = spi_dev;
    g_vol_reg = 0;

    /*
     * The SPI driver auto-configures CS GPIOs listed in the
     * controller's cs-gpios DT property, but XCS and XDCS come
     * from spi_config.cs — which the driver toggles but never
     * configures.  Set them up as outputs (inactive = deasserted)
     * before any SPI transactions.
     */
    int ret = gpio_pin_configure_dt(&g_vs_cmd_spi_cfg.cs.gpio,
                                    GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure XCS: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&g_vs_data_spi_cfg.cs.gpio,
                                GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure XDCS: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&g_vs_dreq, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure DREQ: %d", ret);
        return ret;
    }

    ret = vs_write_reg(VS_REG_MODE, VS_MODE_SDINEW | VS_MODE_RESET);
    if (ret) {
        return ret;
    }
    k_msleep(5);
    ret = vs_wait_dreq();
    if (ret) {
        return ret;
    }

    /*
     * SCI_CLOCKF = 0x8800: XTALI x 3.5 = 43 MHz internal clock.
     * Allows faster SPI data transfers and reduces codec latency.
     */
    ret = vs_write_reg(VS_REG_CLOCKF, 0x8800);
    if (ret) {
        return ret;
    }
    k_msleep(2);

    ret = vs1053b_set_volume(80);
    if (ret) {
        return ret;
    }

    uint16_t status;
    ret = vs_read_reg(VS_REG_STATUS, &status);
    if (ret) {
        return ret;
    }

    LOG_INF("VS1053B status: 0x%04x", status);
    return 0;
}
