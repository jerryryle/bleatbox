/*
 * LIS2DW12 accelerometer — wakeup event producer.
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

#define LIS2DW12_REG_CTRL1 0x20

/*
 * 100 Hz, low-power mode 1: ODR[7:4]=0101, MODE[3:2]=00, LP_MODE[1:0]=01.
 */
#define LIS2DW12_ODR_100HZ_LP1 0x51

static const struct device *g_accel_dev =
    DEVICE_DT_GET(DT_NODELABEL(lis2dw12));

static const struct i2c_dt_spec g_i2c_spec =
    I2C_DT_SPEC_GET(DT_NODELABEL(lis2dw12));

static struct k_msgq *g_evt_q;

/* Set in accel_init() once the device is confirmed ready; gates
 * accel_start() so it never touches an unready peripheral. */
static bool g_ready;

/*
 * The sensor API stores this pointer (the LIS2DW12 driver hands it
 * back on every interrupt), so it must outlive accel_init().
 */
static const struct sensor_trigger g_motion_trigger = {
    .type = SENSOR_TRIG_MOTION,
    .chan = SENSOR_CHAN_ACCEL_XYZ,
};

static void accel_trigger_handler(const struct device *dev,
                                  const struct sensor_trigger *trig)
{
    struct event evt = {.type = EVENT_VIBRATION};
    k_msgq_put(g_evt_q, &evt, K_NO_WAIT);
}

int accel_init(struct k_msgq *event_q)
{
    g_evt_q = event_q;
    g_ready = device_is_ready(g_accel_dev);

    if (!g_ready) {
        LOG_ERR("LIS2DW12 device not ready");
        return -ENODEV;
    }

    LOG_INF("LIS2DW12 initialized");
    return 0;
}

int accel_set_threshold(uint16_t threshold_mg)
{
    if (!g_ready) {
        return -ENODEV;
    }

    /* Set wakeup threshold via the driver's attr interface.
     * Convert mg to m/s²: val = mg * 9.807 / 1000. */
    uint32_t ums2 = (uint32_t)threshold_mg * 9807;
    struct sensor_value threshold_val = {
        .val1 = ums2 / 1000000,
        .val2 = ums2 % 1000000,
    };

    int ret = sensor_attr_set(g_accel_dev, SENSOR_CHAN_ACCEL_XYZ,
                              SENSOR_ATTR_UPPER_THRESH, &threshold_val);
    if (ret) {
        LOG_ERR("Failed to set wakeup threshold: %d", ret);
    }
    return ret;
}

int accel_start(uint16_t threshold_mg)
{
    if (!g_ready) {
        return -ENODEV;
    }

    int ret = accel_set_threshold(threshold_mg);
    if (ret) {
        return ret;
    }

    /* Register wakeup trigger — begins producing EVENT_VIBRATION. */
    ret = sensor_trigger_set(g_accel_dev, &g_motion_trigger,
                             accel_trigger_handler);
    if (ret) {
        LOG_ERR("Failed to set trigger: %d", ret);
        return ret;
    }

    LOG_INF("LIS2DW12 started — threshold %u mg", threshold_mg);
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
    int ret = i2c_reg_read_byte_dt(&g_i2c_spec, LIS2DW12_REG_CTRL1,
                                   prev_ctrl1);
    if (ret) {
        return ret;
    }
    return i2c_reg_write_byte_dt(&g_i2c_spec, LIS2DW12_REG_CTRL1,
                                 LIS2DW12_ODR_100HZ_LP1);
}

int accel_odr_restore(uint8_t prev_ctrl1)
{
    return i2c_reg_write_byte_dt(&g_i2c_spec, LIS2DW12_REG_CTRL1,
                                 prev_ctrl1);
}
