/*
 * Message composition — random sound/delay for each slot.
 */

#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <zephyr/random/random.h>

#include "compose.h"
#include "message.h"
#include "wrap_to_range.h"

static uint16_t g_delay_min_ms;
static uint16_t g_delay_max_ms;
static uint8_t g_first_sound;
static uint8_t g_num_sounds;
static bool g_initialized;

void compose_init(uint16_t delay_min_ms, uint16_t delay_max_ms,
                  uint8_t first_sound, uint8_t num_sounds)
{
    g_delay_min_ms = delay_min_ms;
    g_delay_max_ms = delay_max_ms;
    g_first_sound = first_sound;
    g_num_sounds = num_sounds;
    g_initialized = true;
}

void compose_set_delay(uint16_t delay_min_ms, uint16_t delay_max_ms)
{
    g_delay_min_ms = delay_min_ms;
    g_delay_max_ms = delay_max_ms;
}

void compose_get_delay(uint16_t *delay_min_ms, uint16_t *delay_max_ms)
{
    *delay_min_ms = g_delay_min_ms;
    *delay_max_ms = g_delay_max_ms;
}

int compose_message(uint8_t payload[16])
{
    if (!g_initialized) {
        return -EINVAL;
    }
    /* Need at least the local-only sound plus one assignable sound. */
    if (g_num_sounds < 2) {
        return -ENODATA;
    }

    /* Leaves seq/reserved bits zero; the mesh header seq is authoritative. */
    memset(payload, 0, 16);

    for (int slot = 0; slot < MESSAGE_SLOTS; slot++) {
        uint8_t sound = (uint8_t)wrap_to_range(sys_rand32_get(),
                                               g_first_sound,
                                               g_num_sounds - 1);
        uint16_t delay = (uint16_t)wrap_to_range(sys_rand32_get(),
                                                 g_delay_min_ms,
                                                 g_delay_max_ms);
        /* Boxes only ever compose goat sounds; misc comes from a Mac. */
        message_pack_slot(payload, slot, sound, MESSAGE_TYPE_GOAT, delay);
    }

    message_set_command(payload, MESSAGE_CMD_PLAY);
    return 0;
}
