/*
 * Assignment generation — random sound/delay for each peer device.
 */

#ifndef ASSIGNMENTS_H_
#define ASSIGNMENTS_H_

#include <stdint.h>
#include "ble.h"

/**
 * Store peer and delay configuration for future assignments.
 *
 * @param peer_ids      Array of peer device IDs (copied internally).
 * @param num_peers     Length of @p peer_ids.
 * @param delay_min_ms  Minimum random delay in milliseconds.
 * @param delay_max_ms  Maximum random delay in milliseconds.
 */
void assignments_init(const uint8_t *peer_ids, uint8_t num_peers,
		      uint16_t delay_min_ms, uint16_t delay_max_ms);

/**
 * Generate random sound and delay assignments for the configured peers.
 *
 * @param out  Output array (must have room for at least num_peers entries).
 * @return     Number of assignments generated, or negative errno on error.
 */
int assignments_generate(struct ble_assignment *out);

#endif /* ASSIGNMENTS_H_ */
