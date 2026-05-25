/*
 * Assignment generation — random sound/delay for each peer device.
 */

#ifndef ASSIGNMENTS_H_
#define ASSIGNMENTS_H_

#include <stdint.h>
#include "ble.h"

/**
 * Generate random sound and delay assignments for a set of devices.
 *
 * @param device_ids   Array of peer device IDs.
 * @param num_devices  Length of @p device_ids.
 * @param out          Output array (must have room for @p num_devices entries).
 */
void assignments_generate(const uint8_t *device_ids, uint8_t num_devices,
			  struct ble_assignment *out);

#endif /* ASSIGNMENTS_H_ */
