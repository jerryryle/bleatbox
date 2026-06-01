/*
 * LIS2DH12 accelerometer — any-motion event producer.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "accel.h"
#include "events.h"

LOG_MODULE_REGISTER(accel, LOG_LEVEL_INF);

#define LIS2DH_REG_CTRL1 0x20
#define LIS2DH_REG_CTRL2 0x21
#define LIS2DH_REG_REFERENCE 0x26
#define LIS2DH_HPIS1 BIT(0)
#define LIS2DH_ODR_100HZ_LP 0x57 /* ODR=100Hz, low-power, XYZ enabled */

static const struct device *g_accel_dev =
    DEVICE_DT_GET(DT_NODELABEL(lis2dh12));

static const struct i2c_dt_spec g_i2c_spec =
    I2C_DT_SPEC_GET(DT_NODELABEL(lis2dh12));

static struct k_msgq *g_evt_q;

static void accel_trigger_handler(const struct device *dev,
                                  const struct sensor_trigger *trig)
{
    struct event evt = {.type = EVENT_VIBRATION};
    k_msgq_put(g_evt_q, &evt, K_NO_WAIT);
}

int accel_init(struct k_msgq *event_q, uint16_t threshold_mg)
{
    g_evt_q = event_q;

    if (!device_is_ready(g_accel_dev)) {
        LOG_ERR("LIS2DH12 device not ready");
        return -ENODEV;
    }

    /* Set any-motion threshold */
    uint32_t ums2 = (uint32_t)threshold_mg * 9807;
    struct sensor_value threshold_val = {
        .val1 = ums2 / 1000000,
        .val2 = ums2 % 1000000,
    };

    int ret = sensor_attr_set(g_accel_dev, SENSOR_CHAN_ACCEL_XYZ,
                              SENSOR_ATTR_SLOPE_TH, &threshold_val);
    if (ret) {
        LOG_ERR("Failed to set slope threshold: %d", ret);
        return ret;
    }

    /* Require 1 sample above threshold to trigger */
    struct sensor_value dur_val = {.val1 = 1};
    ret = sensor_attr_set(g_accel_dev, SENSOR_CHAN_ACCEL_XYZ,
                          SENSOR_ATTR_SLOPE_DUR, &dur_val);
    if (ret) {
        LOG_ERR("Failed to set slope duration: %d", ret);
        return ret;
    }

    /* Register any-motion trigger */
    struct sensor_trigger trig = {
        .type = SENSOR_TRIG_DELTA,
        .chan = SENSOR_CHAN_ACCEL_XYZ,
    };

    ret = sensor_trigger_set(g_accel_dev, &trig, accel_trigger_handler);
    if (ret) {
        LOG_ERR("Failed to set trigger: %d", ret);
        return ret;
    }

    /*
     * The Zephyr LIS2DH driver doesn't enable the high-pass filter
     * for the interrupt source.  Without it, the threshold is compared
     * against raw acceleration (including gravity), so a 200 mg
     * threshold fires constantly from the ~1 g Z-axis reading.
     * Enable HPIS1 so the interrupt sees only changes in acceleration.
     */
    ret = i2c_reg_write_byte_dt(&g_i2c_spec, LIS2DH_REG_CTRL2, LIS2DH_HPIS1);
    if (ret) {
        LOG_ERR("Failed to enable HP filter for INT1: %d", ret);
        return ret;
    }

    /* Reading REFERENCE resets the HP filter, setting the current
     * acceleration (including gravity) as the baseline. */
    uint8_t dummy;
    ret = i2c_reg_read_byte_dt(&g_i2c_spec, LIS2DH_REG_REFERENCE, &dummy);
    if (ret) {
        LOG_ERR("Failed to read reference register: %d", ret);
        return ret;
    }

    LOG_INF("LIS2DH12 ready — threshold %u mg", threshold_mg);
    return 0;
}

int accel_sample_xyz(struct sensor_value *x,
                     struct sensor_value *y,
                     struct sensor_value *z)
{
    int ret = sensor_sample_fetch(g_accel_dev);
    if (ret) {
        return ret;
    }

    ret = sensor_channel_get(g_accel_dev, SENSOR_CHAN_ACCEL_X, x);
    if (ret) {
        return ret;
    }
    ret = sensor_channel_get(g_accel_dev, SENSOR_CHAN_ACCEL_Y, y);
    if (ret) {
        return ret;
    }
    return sensor_channel_get(g_accel_dev, SENSOR_CHAN_ACCEL_Z, z);
}

int accel_odr_boost(uint8_t *prev_ctrl1)
{
    int ret = i2c_reg_read_byte_dt(&g_i2c_spec, LIS2DH_REG_CTRL1,
                                   prev_ctrl1);
    if (ret) {
        return ret;
    }
    return i2c_reg_write_byte_dt(&g_i2c_spec, LIS2DH_REG_CTRL1,
                                 LIS2DH_ODR_100HZ_LP);
}

int accel_odr_restore(uint8_t prev_ctrl1)
{
    return i2c_reg_write_byte_dt(&g_i2c_spec, LIS2DH_REG_CTRL1,
                                 prev_ctrl1);
}
