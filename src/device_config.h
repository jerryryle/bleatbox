#ifndef DEVICE_CONFIG_H_
#define DEVICE_CONFIG_H_

#include <stdint.h>

#define DEVICE_CONFIG_MAX_PEERS 30

/**
 * Load device configuration from /SD:/bleatbox.cfg.
 *
 * @return 0 on success, negative errno on failure.
 */
int device_config_load(void);

/** Return this device's configured ID. */
uint8_t device_config_get_id(void);

/**
 * Return the configured peer device ID list.
 *
 * @param[out] count  Number of peers.
 * @return Pointer to the peer ID array.
 */
const uint8_t *device_config_get_peers(uint8_t *count);

#endif /* DEVICE_CONFIG_H_ */
