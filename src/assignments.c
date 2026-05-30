/*
 * Assignment generation — random sound/delay for each peer device.
 */

#include <zephyr/random/random.h>

#include "assignments.h"
#include "clamp.h"
#include "sounds.h"

#define DELAY_MIN_MS  0
#define DELAY_MAX_MS  2000

void assignments_generate(const uint8_t *device_ids, uint8_t num_devices,
			  struct ble_assignment *out)
{
	uint8_t num_sounds = sounds_get_count();

	for (int i = 0; i < num_devices; i++) {
		out[i].device_id = device_ids[i];
		out[i].sound     = (uint8_t)clamp(sys_rand32_get() % num_sounds,
						  1, num_sounds - 1);
		out[i].delay_ms  = (uint16_t)clamp(sys_rand32_get() % (DELAY_MAX_MS + 1),
						   DELAY_MIN_MS, DELAY_MAX_MS);
	}
}
