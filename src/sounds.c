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

static uint8_t g_sound_count;

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

    bool present[100] = {false};
    int max_index = -1;
    g_sound_count = 0;

    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue;
        }

        const char *dot = strrchr(entry.name, '.');
        if (!dot || strcmp(dot, ".mp3") != 0) {
            continue;
        }

        static const char prefix[] = "goat";
        if (strncmp(entry.name, prefix, sizeof(prefix) - 1) != 0) {
            continue;
        }

        const char *num_start = entry.name + sizeof(prefix) - 1;
        size_t num_len = dot - num_start;
        if (num_len == 0 || num_len > 2) {
            continue;
        }

        bool digits = true;
        for (size_t i = 0; i < num_len; i++) {
            if (num_start[i] < '0' || num_start[i] > '9') {
                digits = false;
                break;
            }
        }
        if (!digits) {
            continue;
        }

        char num_str[3] = {0};
        memcpy(num_str, num_start, num_len);
        int idx = (int)strtoul(num_str, NULL, 10);
        if (idx > 99) {
            continue;
        }

        present[idx] = true;
        if (idx > max_index) {
            max_index = idx;
        }
    }

    fs_closedir(&dir);

    if (max_index < 0) {
        LOG_ERR("No .mp3 files found on SD card");
        return -ENOENT;
    } else {
        bool has_gap = false;
        for (int i = 0; i <= max_index; i++) {
            if (!present[i]) {
                LOG_ERR("Missing sound file: goat%02u.mp3", i);
                has_gap = true;
            }
        }
        if (has_gap) {
            return -ENOENT;
        }
        g_sound_count = (uint8_t)(max_index + 1);
        LOG_INF("Found sounds goat00-goat%02u (%u total)", max_index,
                g_sound_count);
    }

    return 0;
}

uint8_t sounds_get_count(void)
{
    return g_sound_count;
}

int sounds_get_path(uint8_t index, char *buf, size_t len)
{
    if (index >= g_sound_count) {
        return -EINVAL;
    }

    int need = snprintf(buf, len, SDCARD_MOUNT_POINT "/goat%02u.mp3", index);
    if (need < 0 || (size_t)need >= len) {
        return -ENOSPC;
    }

    return 0;
}
