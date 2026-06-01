/*
 * Sound discovery — scan the SD card for numbered MP3 files.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

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

        size_t num_len = dot - entry.name;
        if (num_len == 0 || num_len > 2) {
            continue;
        }

        bool digits = true;
        for (size_t i = 0; i < num_len; i++) {
            if (entry.name[i] < '0' || entry.name[i] > '9') {
                digits = false;
                break;
            }
        }
        if (!digits) {
            continue;
        }

        char num_str[3] = {0};
        memcpy(num_str, entry.name, num_len);
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
                LOG_ERR("Missing sound file: %02u.mp3", i);
                has_gap = true;
            }
        }
        if (has_gap) {
            return -ENOENT;
        }
        g_sound_count = (uint8_t)(max_index + 1);
        LOG_INF("Found sounds 00-%02u (%u total)", max_index,
                g_sound_count);
    }

    return 0;
}

uint8_t sounds_get_count(void)
{
    return g_sound_count;
}
