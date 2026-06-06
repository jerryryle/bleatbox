/*
 * Broadcast log core — pure data structure logic with no platform
 * dependencies.  The locking wrapper lives in broadcast_log.c.
 */

#include <string.h>

#include "broadcast_log_core.h"

static bool check(const struct broadcast_log *log,
                  uint8_t originator, uint8_t seq)
{
    for (int i = 0; i < log->count; i++) {
        if (log->entries[i].originator == originator &&
            log->entries[i].seq == seq) {
            return true;
        }
    }
    return false;
}

static void record(struct broadcast_log *log,
                   uint8_t originator, uint8_t seq)
{
    log->entries[log->next].originator = originator;
    log->entries[log->next].seq = seq;
    log->next = (log->next + 1) % BROADCAST_LOG_SIZE;
    if (log->count < BROADCAST_LOG_SIZE) {
        log->count++;
    }
}

void broadcast_log_core_init(struct broadcast_log *log)
{
    memset(log->entries, 0, sizeof(log->entries));
    log->next = 0;
    log->count = 0;
}

bool broadcast_log_core_check_and_record(struct broadcast_log *log,
                                         uint8_t originator, uint8_t seq)
{
    bool duplicate = check(log, originator, seq);
    if (!duplicate) {
        record(log, originator, seq);
    }
    return duplicate;
}

void broadcast_log_core_record(struct broadcast_log *log,
                               uint8_t originator, uint8_t seq)
{
    record(log, originator, seq);
}
