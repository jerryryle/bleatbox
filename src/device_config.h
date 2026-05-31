#ifndef DEVICE_CONFIG_H_
#define DEVICE_CONFIG_H_

#include <stdint.h>

#define DEVICE_CONFIG_MAX_PEERS 30

struct device_config {
	uint8_t id;
	uint8_t peers[DEVICE_CONFIG_MAX_PEERS];
	uint8_t peer_count;
	uint8_t volume;
	uint16_t delay_min_ms;
	uint16_t delay_max_ms;
	uint16_t accel_threshold_mg;
};

/**
 * Load device configuration from /SD:/bleatbox.cfg.
 *
 * @param[out] cfg  Populated on success.
 * @return 0 on success, negative errno on failure.
 */
int device_config_load(struct device_config *cfg);

#endif /* DEVICE_CONFIG_H_ */
