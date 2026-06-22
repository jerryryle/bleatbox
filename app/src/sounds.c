/*
 * Sound discovery — scan the SD card for numbered MP3 files.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sounds.h"
#include "sounds_match.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(sounds, LOG_LEVEL_INF);

static uint8_t g_goat_count;
static uint8_t g_misc_count;

void sounds_init(void)
{
    g_goat_count = 0;
    g_misc_count = 0;
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

    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue;
        }

        const char *dot = strrchr(entry.name, '.');
        if (!dot || strcmp(dot, ".mp3") != 0) {
            continue;
        }

        int idx;

        idx = sounds_try_match(entry.name, dot, "goat", 4);
        if (idx >= 0) {
            goat_present[idx] = true;
            if (idx > goat_max) {
                goat_max = idx;
            }
            continue;
        }

        idx = sounds_try_match(entry.name, dot, "misc", 4);
        if (idx >= 0) {
            misc_present[idx] = true;
            if (idx > misc_max) {
                misc_max = idx;
            }
        }
    }

    fs_closedir(&dir);

    g_goat_count = sounds_validate_set(goat_present, goat_max);
    if (g_goat_count == 0) {
        for (int i = 0; i <= goat_max; i++) {
            if (!goat_present[i]) {
                LOG_ERR("Missing sound file: goat%02d.mp3", i);
            }
        }
        LOG_ERR("No goat sounds found on SD card (or missing sounds encountered)");
        return -ENOENT;
    }
    LOG_INF("Found sounds goat00-goat%02d (%u total)", goat_max, g_goat_count);

    g_misc_count = sounds_validate_set(misc_present, misc_max);
    if (g_misc_count == 0 && misc_max >= 0) {
        for (int i = 0; i <= misc_max; i++) {
            if (!misc_present[i]) {
                LOG_WRN("Missing sound file: misc%02d.mp3", i);
            }
        }
        LOG_WRN("No misc sounds found on SD card (or missing sounds encountered)");
    } else if (g_misc_count > 0) {
        LOG_INF("Found sounds misc00-misc%02d (%u total)",
                misc_max, g_misc_count);
    }

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
