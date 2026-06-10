#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>

#include "device_config.h"
#include "device_config_parse.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(device_config, LOG_LEVEL_INF);

#define CONFIG_PATH SDCARD_MOUNT_POINT "/bleatbox.cfg"
#define LINE_MAX 256

int device_config_load(struct device_config *cfg)
{
    *cfg = (struct device_config){
        .volume = 80,
        .delay_min_ms = 0,
        .delay_max_ms = 2000,
        .accel_threshold_mg = 200,
        .relay_ttl = 2,
    };

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
    if (cfg->peer_count == 0) {
        LOG_ERR("Config file missing 'peers'");
        return -EINVAL;
    }
    if (cfg->delay_min_ms > cfg->delay_max_ms) {
        LOG_ERR("delay_min (%u) must not exceed delay_max (%u)",
                cfg->delay_min_ms, cfg->delay_max_ms);
        return -EINVAL;
    }

    LOG_INF("Device ID: 0x%02x", cfg->id);
    LOG_HEXDUMP_INF(cfg->peers, cfg->peer_count, "Peers:");
    LOG_INF("Delay range: %u–%u ms", cfg->delay_min_ms, cfg->delay_max_ms);
    LOG_INF("Accel threshold: %u mg", cfg->accel_threshold_mg);

    return 0;
}
