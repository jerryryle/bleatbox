/*
 * Battery monitor — one-shot SAADC reads of the VBAT divider.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/voltage_divider.h>
#include <zephyr/logging/log.h>

#include "battery.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

/* Conversions averaged per read.  A single 12-bit SAADC sample carries a
 * few LSB of noise; averaging 16 cuts the spread ~4x while still taking
 * well under a millisecond. */
#define BATTERY_SAMPLE_COUNT 16

static const struct voltage_divider_dt_spec g_vbatt =
    VOLTAGE_DIVIDER_DT_SPEC_GET(DT_PATH(vbatt));
static bool g_ready;

int battery_init(void)
{
    g_ready = false;

    if (!adc_is_ready_dt(&g_vbatt.port)) {
        LOG_ERR("ADC controller not ready");
        return -ENODEV;
    }

    int ret = adc_channel_setup_dt(&g_vbatt.port);
    if (ret) {
        LOG_ERR("ADC channel setup failed: %d", ret);
        return ret;
    }

    g_ready = true;
    return 0;
}

int battery_read_mv(int32_t *millivolts)
{
    if (!g_ready) {
        return -ENODEV;
    }

    int16_t raw;
    struct adc_sequence seq = {
        .buffer = &raw,
        .buffer_size = sizeof(raw),
    };

    int ret = adc_sequence_init_dt(&g_vbatt.port, &seq);
    if (ret) {
        return ret;
    }

    /* Average several conversions to smooth out per-sample noise. */
    int32_t sum = 0;
    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        ret = adc_read_dt(&g_vbatt.port, &seq);
        if (ret) {
            return ret;
        }
        sum += raw;
    }

    /* Convert the averaged raw count to the voltage at the ADC pin, then
     * undo the divider to recover the battery voltage. */
    int32_t mv = (sum + BATTERY_SAMPLE_COUNT / 2) / BATTERY_SAMPLE_COUNT;
    ret = adc_raw_to_millivolts_dt(&g_vbatt.port, &mv);
    if (ret) {
        return ret;
    }

    ret = voltage_divider_scale_dt(&g_vbatt, &mv);
    if (ret) {
        return ret;
    }

    *millivolts = mv;
    return 0;
}
