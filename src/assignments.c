/*
 * Assignment generation — random sound/delay for each peer device.
 */

#include <string.h>
#include <errno.h>
#include <zephyr/random/random.h>

#include "assignments.h"
#include "clamp.h"

#define MAX_PEERS 30

static struct assignment g_assignments[MAX_PEERS];
static uint8_t g_peer_ids[MAX_PEERS];
static uint8_t g_num_peers;
static uint16_t g_delay_min_ms;
static uint16_t g_delay_max_ms;
static uint8_t g_num_sounds;
static bool g_initialized;

void assignments_init(const uint8_t *ids, uint8_t count,
                      uint16_t min_ms, uint16_t max_ms,
                      uint8_t num_sounds)
{
    g_num_peers = count < MAX_PEERS ? count : MAX_PEERS;
    memcpy(g_peer_ids, ids, g_num_peers);
    g_delay_min_ms = min_ms;
    g_delay_max_ms = max_ms;
    g_num_sounds = num_sounds;
    g_initialized = true;
}

int assignments_generate(const struct assignment **out)
{
    if (!g_initialized) {
        return -EINVAL;
    }
    if (g_num_sounds < 2) {
        return 0;
    }

    for (int i = 0; i < g_num_peers; i++) {
        g_assignments[i].device_id = g_peer_ids[i];
        g_assignments[i].sound = (uint8_t)clamp(sys_rand32_get() % g_num_sounds,
                                                1, g_num_sounds - 1);
        g_assignments[i].delay_ms = (uint16_t)clamp(
            sys_rand32_get() % (g_delay_max_ms + 1),
            g_delay_min_ms, g_delay_max_ms);
    }

    *out = g_assignments;
    return g_num_peers;
}
