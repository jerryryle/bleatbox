/*
 * Sound discovery — scan the SD card for numbered MP3 files.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sounds.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(sounds, LOG_LEVEL_INF);

static uint8_t g_goat_count;
static uint8_t g_misc_count;

struct sound_prefix {
    const char *name;
    size_t len;
    bool *present;
    int max_index;
};

/*
 * Try to match a filename against a prefix and extract the numeric
 * index.  Returns the index on success, -1 on no match.
 */
static int try_match(const char *filename, const char *dot,
                     const char *prefix, size_t prefix_len)
{
    if (strncmp(filename, prefix, prefix_len) != 0) {
        return -1;
    }

    const char *num_start = filename + prefix_len;
    size_t num_len = dot - num_start;
    if (num_len == 0 || num_len > 2) {
        return -1;
    }

    for (size_t i = 0; i < num_len; i++) {
        if (num_start[i] < '0' || num_start[i] > '9') {
            return -1;
        }
    }

    char num_str[3] = {0};
    memcpy(num_str, num_start, num_len);
    int idx = (int)strtoul(num_str, NULL, 10);
    if (idx > 99) {
        return -1;
    }

    return idx;
}

static uint8_t validate_set(const char *name, bool *present, int max_index)
{
    if (max_index < 0) {
        return 0;
    }

    bool has_gap = false;
    for (int i = 0; i <= max_index; i++) {
        if (!present[i]) {
            LOG_ERR("Missing sound file: %s%02u.mp3", name, i);
            has_gap = true;
        }
    }

    if (has_gap) {
        return 0;
    }

    uint8_t count = (uint8_t)(max_index + 1);
    LOG_INF("Found sounds %s00-%s%02u (%u total)", name, name,
            max_index, count);
    return count;
}

int sounds_scan(void)
{
    struct fs_dir_t dir;
    struct fs_dirent entry;

    fs_dir_t_init(&dir);
    int ret = fs_opendir(&dir, SDCARD_MOUNT_POINT);
    if (ret < 0) {
        LOG_ERR("Cannot open SD root: %d", ret);
        return ret;
    }

    bool goat_present[100] = {false};
    bool misc_present[100] = {false};
    int goat_max = -1;
    int misc_max = -1;

    g_goat_count = 0;
    g_misc_count = 0;

    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue;
        }

        const char *dot = strrchr(entry.name, '.');
        if (!dot || strcmp(dot, ".mp3") != 0) {
            continue;
        }

        int idx;

        idx = try_match(entry.name, dot, "goat", 4);
        if (idx >= 0) {
            goat_present[idx] = true;
            if (idx > goat_max) {
                goat_max = idx;
            }
            continue;
        }

        idx = try_match(entry.name, dot, "misc", 4);
        if (idx >= 0) {
            misc_present[idx] = true;
            if (idx > misc_max) {
                misc_max = idx;
            }
        }
    }

    fs_closedir(&dir);

    g_goat_count = validate_set("goat", goat_present, goat_max);
    if (g_goat_count == 0) {
        LOG_ERR("No goat sounds found on SD card");
        return -ENOENT;
    }

    g_misc_count = validate_set("misc", misc_present, misc_max);

    return 0;
}

uint8_t sounds_get_count(enum sound_type type)
{
    switch (type) {
    case SOUND_TYPE_GOAT:
        return g_goat_count;
    case SOUND_TYPE_MISC:
        return g_misc_count;
    default:
        return 0;
    }
}

static const char *type_prefix(enum sound_type type)
{
    switch (type) {
    case SOUND_TYPE_GOAT:
        return "goat";
    case SOUND_TYPE_MISC:
        return "misc";
    default:
        return "";
    }
}

int sounds_get_path(enum sound_type type, uint8_t index,
                    char *buf, size_t len)
{
    if (index >= sounds_get_count(type)) {
        return -EINVAL;
    }

    int need = snprintf(buf, len, SDCARD_MOUNT_POINT "/%s%02u.mp3",
                        type_prefix(type), index);
    if (need < 0 || (size_t)need >= len) {
        return -ENOSPC;
    }

    return 0;
}
