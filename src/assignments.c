/*
 * Assignment generation — random sound/delay for each peer device.
 */

#include <string.h>
#include <errno.h>
#include <zephyr/random/random.h>

#include "assignments.h"
#include "clamp.h"
#include "sounds.h"

#define MAX_PEERS 30

static uint8_t peer_ids[MAX_PEERS];
static uint8_t num_peers;
static uint16_t delay_min_ms;
static uint16_t delay_max_ms;
static bool initialized;

void assignments_init(const uint8_t *ids, uint8_t count,
		      uint16_t min_ms, uint16_t max_ms)
{
	num_peers = count < MAX_PEERS ? count : MAX_PEERS;
	memcpy(peer_ids, ids, num_peers);
	delay_min_ms = min_ms;
	delay_max_ms = max_ms;
	initialized = true;
}

int assignments_generate(struct ble_assignment *out)
{
	if (!initialized) {
		return -EINVAL;
	}

	uint8_t num_sounds = sounds_get_count();

	for (int i = 0; i < num_peers; i++) {
		out[i].device_id = peer_ids[i];
		out[i].sound     = (uint8_t)clamp(sys_rand32_get() % num_sounds,
						  1, num_sounds - 1);
		out[i].delay_ms  = (uint16_t)clamp(sys_rand32_get() % (delay_max_ms + 1),
						   delay_min_ms, delay_max_ms);
	}

	return num_peers;
}
