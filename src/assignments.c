/*
 * Assignment generation — random sound/delay for each peer device.
 */

#include <zephyr/random/random.h>

#include "assignments.h"

#define NUM_SOUNDS    10
#define DELAY_MIN_MS  0
#define DELAY_MAX_MS  2000

void assignments_generate(const uint8_t *device_ids, uint8_t num_devices,
			  struct ble_assignment *out)
{
	for (int i = 0; i < num_devices; i++) {
		out[i].device_id = device_ids[i];
		out[i].sound     = (uint8_t)(sys_rand32_get() % NUM_SOUNDS);
		out[i].delay_ms  = DELAY_MIN_MS +
			(uint16_t)(sys_rand32_get() %
				   (DELAY_MAX_MS - DELAY_MIN_MS + 1));
	}
}
