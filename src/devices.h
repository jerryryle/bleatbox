/*
 * Network device list — IDs of the OTHER devices in the network.
 *
 * Each entry is the lower byte of FICR->DEVICEID[0] for one peer.
 * This device's own ID must NOT appear here.  Populate with actual
 * values read from each board (see README.md).
 */

#ifndef DEVICES_H_
#define DEVICES_H_

#include <stdint.h>

static const uint8_t OTHER_KNOWN_DEVICE_IDS[] = {
	0x00, 0x01, 0x02, 0x03, 0x04,
};
#define NUM_OTHER_DEVICES ARRAY_SIZE(OTHER_KNOWN_DEVICE_IDS)

#endif /* DEVICES_H_ */
