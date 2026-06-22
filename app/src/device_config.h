#ifndef DEVICE_CONFIG_H_
#define DEVICE_CONFIG_H_

#include <stdint.h>

struct device_config {
    uint8_t id;
    uint8_t volume;
    uint16_t delay_min_ms;
    uint16_t delay_max_ms;
    uint16_t accel_threshold_mg;
    uint8_t relay_ttl;
};

/**
 * Populate @p cfg with built-in defaults (default id).
 *
 * Used by device_config_load() before parsing, and by callers that need
 * a usable config when the SD card or config file is unavailable.
 */
void device_config_defaults(struct device_config *cfg);

/**
 * Load device configuration from /SD:/bleatbox.cfg.
 *
 * @param[out] cfg  Populated on success.
 * @return 0 on success, negative errno on failure.
 */
int device_config_load(struct device_config *cfg);

#endif /* DEVICE_CONFIG_H_ */
