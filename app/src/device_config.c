#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>

#include "device_config.h"
#include "device_config_parse.h"
#include "message.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(device_config, LOG_LEVEL_INF);

#define CONFIG_PATH SDCARD_MOUNT_POINT "/bleatbox.cfg"
#define LINE_MAX 256

#define DEVICE_CONFIG_DEFAULT_ID 0xFF
#define DEVICE_CONFIG_DEFAULT_VOLUME 80
#define DEVICE_CONFIG_DEFAULT_DELAY_MIN_MS 0
#define DEVICE_CONFIG_DEFAULT_DELAY_MAX_MS 2000
#define DEVICE_CONFIG_DEFAULT_ACCEL_THRESHOLD_MG 200
#define DEVICE_CONFIG_DEFAULT_RELAY_TTL 6


void device_config_defaults(struct device_config *cfg)
{
    *cfg = (struct device_config){
        .id = DEVICE_CONFIG_DEFAULT_ID,
        .volume = DEVICE_CONFIG_DEFAULT_VOLUME,
        .delay_min_ms = DEVICE_CONFIG_DEFAULT_DELAY_MIN_MS,
        .delay_max_ms = DEVICE_CONFIG_DEFAULT_DELAY_MAX_MS,
        .accel_threshold_mg = DEVICE_CONFIG_DEFAULT_ACCEL_THRESHOLD_MG,
        .relay_ttl = DEVICE_CONFIG_DEFAULT_RELAY_TTL,
    };
}

int device_config_load(struct device_config *cfg)
{
    device_config_defaults(cfg);

    bool has_id = false;

    struct fs_file_t file;
    fs_file_t_init(&file);

    int ret = fs_open(&file, CONFIG_PATH, FS_O_READ);
    if (ret) {
        LOG_ERR("Failed to open %s: %d", CONFIG_PATH, ret);
        return ret;
    }

    char buf[LINE_MAX];
    int pos = 0;
    ssize_t nread;

    while ((nread = fs_read(&file, buf + pos, 1)) == 1) {
        if (buf[pos] == '\n') {
            buf[pos + 1] = '\0';
            ret = device_config_parse_line(buf, cfg, &has_id);
            if (ret) {
                LOG_ERR("Config parse error (%d): %s", ret, buf);
                fs_close(&file);
                return ret;
            }
            pos = 0;
        } else if (pos >= LINE_MAX - 2) {
            /* Truncating and parsing the prefix could silently yield a
             * plausible-but-wrong value — reject the file instead. */
            LOG_ERR("Config line too long (max %d chars)", LINE_MAX - 2);
            fs_close(&file);
            return -EINVAL;
        } else {
            pos++;
        }
    }

    if (nread < 0) {
        /* A mid-file I/O error is not EOF — a truncated config could
         * silently load with defaults for the missing lines. */
        LOG_ERR("Read error in %s: %d", CONFIG_PATH, (int)nread);
        fs_close(&file);
        return (int)nread;
    }

    if (pos > 0) {
        buf[pos] = '\0';
        ret = device_config_parse_line(buf, cfg, &has_id);
        if (ret) {
            LOG_ERR("Config parse error (%d): %s", ret, buf);
            fs_close(&file);
            return ret;
        }
    }

    fs_close(&file);

    if (!has_id) {
        LOG_ERR("Config file missing 'id'");
        return -EINVAL;
    }
    if (cfg->id == MESSAGE_EXT_ORIGINATOR) {
        /* Reserved as the originator of macOS-originated messages; a device
         * using it would collide with that message's mesh dedup key. */
        LOG_ERR("Config 'id' 0x%02x is reserved", MESSAGE_EXT_ORIGINATOR);
        return -EINVAL;
    }
    if (cfg->delay_min_ms > cfg->delay_max_ms) {
        LOG_ERR("delay_min (%u) must not exceed delay_max (%u)",
                cfg->delay_min_ms, cfg->delay_max_ms);
        return -EINVAL;
    }

    LOG_INF("Device ID: 0x%02x", cfg->id);
    LOG_INF("Delay range: %u–%u ms", cfg->delay_min_ms, cfg->delay_max_ms);
    LOG_INF("Accel threshold: %u mg", cfg->accel_threshold_mg);

    return 0;
}
