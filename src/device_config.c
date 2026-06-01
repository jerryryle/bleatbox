#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>

#include "device_config.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(device_config, LOG_LEVEL_INF);

#define CONFIG_PATH SDCARD_MOUNT_POINT "/bleatbox.cfg"
#define LINE_MAX 256

static int parse_hex_byte(const char *s, uint8_t *out)
{
    char *end;
    unsigned long val = strtoul(s, &end, 16);

    if (end == s || val > 0xFF) {
        return -EINVAL;
    }
    *out = (uint8_t)val;
    return 0;
}

static int parse_line(char *line, struct device_config *cfg, bool *has_id)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    if (*line == '#' || *line == '\n' || *line == '\r' || *line == '\0') {
        return 0;
    }

    char *saveptr;
    char *key = strtok_r(line, " \t\r\n", &saveptr);

    if (!key) {
        return 0;
    }

    if (strcmp(key, "id") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!val) {
            LOG_ERR("'id' requires a value");
            return -EINVAL;
        }
        if (parse_hex_byte(val, &cfg->id)) {
            LOG_ERR("Invalid id value: %s", val);
            return -EINVAL;
        }
        *has_id = true;
    } else if (strcmp(key, "peers") == 0) {
        cfg->peer_count = 0;
        char *val;

        while ((val = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
            if (cfg->peer_count >= DEVICE_CONFIG_MAX_PEERS) {
                LOG_ERR("Too many peers (max %d)",
                        DEVICE_CONFIG_MAX_PEERS);
                return -ENOSPC;
            }
            if (parse_hex_byte(val, &cfg->peers[cfg->peer_count])) {
                LOG_ERR("Invalid peer value: %s", val);
                return -EINVAL;
            }
            cfg->peer_count++;
        }
        if (cfg->peer_count == 0) {
            LOG_ERR("'peers' requires at least one value");
            return -EINVAL;
        }
    } else if (strcmp(key, "volume") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!val) {
            LOG_ERR("'volume' requires a value");
            return -EINVAL;
        }
        char *end;
        unsigned long v = strtoul(val, &end, 10);
        if (end == val || v > 100) {
            LOG_ERR("Volume must be 0-100, got: %s", val);
            return -EINVAL;
        }
        cfg->volume = (uint8_t)v;
    } else if (strcmp(key, "delay_min") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!val) {
            LOG_ERR("'delay_min' requires a value");
            return -EINVAL;
        }
        char *end;
        unsigned long v = strtoul(val, &end, 10);
        if (end == val || v > UINT16_MAX) {
            LOG_ERR("delay_min must be 0-%u, got: %s", UINT16_MAX, val);
            return -EINVAL;
        }
        cfg->delay_min_ms = (uint16_t)v;
    } else if (strcmp(key, "delay_max") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!val) {
            LOG_ERR("'delay_max' requires a value");
            return -EINVAL;
        }
        char *end;
        unsigned long v = strtoul(val, &end, 10);
        if (end == val || v > UINT16_MAX) {
            LOG_ERR("delay_max must be 0-%u, got: %s", UINT16_MAX, val);
            return -EINVAL;
        }
        cfg->delay_max_ms = (uint16_t)v;
    } else if (strcmp(key, "accel_threshold") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!val) {
            LOG_ERR("'accel_threshold' requires a value");
            return -EINVAL;
        }
        char *end;
        unsigned long v = strtoul(val, &end, 10);
        if (end == val || v == 0 || v > UINT16_MAX) {
            LOG_ERR("accel_threshold must be 1-%u mg, got: %s",
                    UINT16_MAX, val);
            return -EINVAL;
        }
        cfg->accel_threshold_mg = (uint16_t)v;
    } else {
        LOG_WRN("Unknown config key: %s", key);
    }

    return 0;
}

int device_config_load(struct device_config *cfg)
{
    *cfg = (struct device_config){
        .volume = 80,
        .delay_max_ms = 2000,
        .accel_threshold_mg = 200,
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
            ret = parse_line(buf, cfg, &has_id);
            if (ret) {
                fs_close(&file);
                return ret;
            }
            pos = 0;
        } else if (pos >= LINE_MAX - 2) {
            buf[pos + 1] = '\0';
            ret = parse_line(buf, cfg, &has_id);
            if (ret) {
                fs_close(&file);
                return ret;
            }
            pos = 0;
            /* Drain the rest of this overlong line */
            char ch;
            while (fs_read(&file, &ch, 1) == 1 && ch != '\n') {
            }
        } else {
            pos++;
        }
    }

    if (pos > 0) {
        buf[pos] = '\0';
        ret = parse_line(buf, cfg, &has_id);
        if (ret) {
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
