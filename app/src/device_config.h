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

/*
 * Persist a single config value to /SD:/bleatbox.cfg.
 *
 * Each function updates its directive in place (preserving its position next
 * to any associated comment) or appends it if absent, leaving every other line
 * untouched.  The file is rewritten via a temp file and a rename, so a failure
 * mid-write leaves the original config intact.  All return 0 on success or a
 * negative errno on failure (e.g. -ENODEV if the SD card isn't mounted).
 */
int device_config_save_id(uint8_t id);
int device_config_save_volume(uint8_t volume);
int device_config_save_delay(uint16_t delay_min_ms, uint16_t delay_max_ms);
int device_config_save_accel_threshold(uint16_t threshold_mg);
int device_config_save_relay_ttl(uint8_t relay_ttl);

#endif /* DEVICE_CONFIG_H_ */
