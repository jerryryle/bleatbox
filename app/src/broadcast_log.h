#ifndef BROADCAST_LOG_H_
#define BROADCAST_LOG_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize (or reset) the broadcast log.
 */
void broadcast_log_init(void);

/**
 * Check whether (originator, seq) has been seen before, and record it
 * if not.  Atomically locked — safe to call from any thread context.
 *
 * @return true if the pair was already in the log (duplicate).
 */
bool broadcast_log_check_and_record(uint8_t originator, uint8_t seq);

/**
 * Unconditionally record (originator, seq) in the log.
 * Use when the caller knows the entry is new (e.g. its own broadcast).
 */
void broadcast_log_record(uint8_t originator, uint8_t seq);

#endif /* BROADCAST_LOG_H_ */
