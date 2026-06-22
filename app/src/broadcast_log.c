/*
 * Broadcast log — thread-safe wrapper around the lock-free core.
 */

#include <zephyr/kernel.h>

#include "broadcast_log.h"
#include "broadcast_log_core.h"

static struct broadcast_log g_log;
static struct k_spinlock g_lock;

void broadcast_log_init(void)
{
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    broadcast_log_core_init(&g_log);
    k_spin_unlock(&g_lock, key);
}

bool broadcast_log_check_and_record(uint8_t originator, uint8_t seq)
{
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    bool duplicate = broadcast_log_core_check_and_record(&g_log,
                                                         originator, seq);
    k_spin_unlock(&g_lock, key);
    return duplicate;
}

void broadcast_log_record(uint8_t originator, uint8_t seq)
{
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    broadcast_log_core_record(&g_log, originator, seq);
    k_spin_unlock(&g_lock, key);
}
