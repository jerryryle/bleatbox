/*
 * Assignment generation — random sound/delay for each peer device.
 */

#ifndef ASSIGNMENTS_H_
#define ASSIGNMENTS_H_

#include <stdint.h>

/** A single device assignment (sound + delay). */
struct assignment {
    uint8_t device_id;
    uint8_t sound;
    uint16_t delay_ms;
};

/**
 * Store peer, delay, and sound count configuration for future assignments.
 *
 * @param peer_ids      Array of peer device IDs (copied internally).
 * @param num_peers     Length of @p peer_ids.
 * @param delay_min_ms  Minimum random delay in milliseconds.
 * @param delay_max_ms  Maximum random delay in milliseconds.
 * @param first_sound   Lowest sound index to assign (skips local-only sounds).
 * @param num_sounds    Number of available sounds.
 */
void assignments_init(const uint8_t *peer_ids, uint8_t num_peers,
                      uint16_t delay_min_ms, uint16_t delay_max_ms,
                      uint8_t first_sound, uint8_t num_sounds);

/**
 * Generate random sound and delay assignments for the configured peers.
 *
 * @param[out] out  Set to point to the internal assignment array.
 *                  Valid until the next call to assignments_generate().
 * @return     Number of assignments generated, or negative errno on error.
 */
int assignments_generate(const struct assignment **out);

#endif /* ASSIGNMENTS_H_ */
