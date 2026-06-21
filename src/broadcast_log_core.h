#ifndef BROADCAST_LOG_CORE_H_
#define BROADCAST_LOG_CORE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Sized well above the fleet size so a burst of broadcasts from every
 * device can't evict entries whose packets are still being relayed around
 * the mesh.  Also gives the reserved macOS-message originator a seq space
 * (8 bits) far larger than this ring, so a fresh message is never dropped
 * as a stale duplicate.
 */
#define BROADCAST_LOG_SIZE 32

struct broadcast_log_entry {
    uint8_t originator;
    uint8_t seq;
};

struct broadcast_log {
    struct broadcast_log_entry entries[BROADCAST_LOG_SIZE];
    uint8_t next;
    uint8_t count;
};

/**
 * Initialize (or reset) a broadcast log.
 */
void broadcast_log_core_init(struct broadcast_log *log);

/**
 * Check whether (originator, seq) is already in the log, and record
 * it if not.
 *
 * @return true if the pair was already in the log (duplicate).
 */
bool broadcast_log_core_check_and_record(struct broadcast_log *log,
                                         uint8_t originator, uint8_t seq);

/**
 * Unconditionally record (originator, seq) in the log.
 */
void broadcast_log_core_record(struct broadcast_log *log,
                               uint8_t originator, uint8_t seq);

#endif /* BROADCAST_LOG_CORE_H_ */
