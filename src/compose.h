/*
 * Message composition — fill a BLE message payload with a random sound and
 * delay for each slot.  Used when this device originates a broadcast (a
 * tissue pull).
 */

#ifndef COMPOSE_H_
#define COMPOSE_H_

#include <stdint.h>

/**
 * Store the delay range and sound range used to compose future messages.
 *
 * @param delay_min_ms  Minimum random delay in milliseconds.
 * @param delay_max_ms  Maximum random delay in milliseconds.
 * @param first_sound   Lowest sound index to assign (skips local-only sounds).
 * @param num_sounds    Number of available sounds.
 */
void compose_init(uint16_t delay_min_ms, uint16_t delay_max_ms,
                  uint8_t first_sound, uint8_t num_sounds);

/**
 * Fill @p payload with a random (sound, delay) for every slot and set the
 * play command.
 *
 * @param[out] payload  16-byte message payload to populate.
 * @return 0 on success, or a negative errno if not initialized or fewer
 *         than two sounds are available.
 */
int compose_message(uint8_t payload[16]);

#endif /* COMPOSE_H_ */
