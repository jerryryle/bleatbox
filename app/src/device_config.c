#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_config.h"
#include "device_config_parse.h"
#include "message.h"
#include "sdcard.h"

LOG_MODULE_REGISTER(device_config, LOG_LEVEL_INF);

#define CONFIG_PATH SDCARD_MOUNT_POINT "/bleatbox.cfg"
#define CONFIG_TMP_PATH SDCARD_MOUNT_POINT "/bleatbox.tmp"
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

/* Log a parse result; return true if it's fatal and the load must abort.
 * An unrecognized directive only warns so a stale or mistyped line doesn't
 * take down the whole config. */
static bool config_line_fatal(int ret, const char *line)
{
    if (ret < 0) {
        LOG_ERR("Config parse error (%d): %s", ret, line);
        return true;
    }
    if (ret == DEVICE_CONFIG_UNKNOWN_KEY) {
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        LOG_WRN("Ignoring unknown config directive: %s", line);
    }
    return false;
}

int device_config_load(struct device_config *cfg)
{
    device_config_defaults(cfg);

    bool has_id = false;

    struct fs_file_t file;
    fs_file_t_init(&file);

    int ret = fs_open(&file, CONFIG_PATH, FS_O_READ);

    /* No main config but a temp left behind means a save crashed between
     * unlinking the old config and renaming the temp into place.  The temp is
     * fully written and closed before the old config is removed, so it holds a
     * valid config — finish the interrupted rename to heal it, then retry. */
    if (ret == -ENOENT && fs_rename(CONFIG_TMP_PATH, CONFIG_PATH) == 0) {
        LOG_WRN("Recovered config from interrupted save");
        ret = fs_open(&file, CONFIG_PATH, FS_O_READ);
    }

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
            if (config_line_fatal(ret, buf)) {
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
        if (config_line_fatal(ret, buf)) {
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

/* True if @p line's first whitespace-delimited token is @p key. */
static bool line_has_key(const char *line, const char *key)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    size_t keylen = strlen(key);
    if (strncmp(line, key, keylen) != 0) {
        return false;
    }

    char after = line[keylen];
    return after == ' ' || after == '\t' || after == '\r' ||
           after == '\n' || after == '\0';
}

static int write_all(struct fs_file_t *file, const char *data, size_t len)
{
    ssize_t w = fs_write(file, data, len);
    if (w < 0) {
        return (int)w;
    }
    if ((size_t)w != len) {
        return -EIO;
    }
    return 0;
}

/* One directive to write: its key and the full "key value\n" line that should
 * replace it.  replaced is bookkeeping for the rewrite pass. */
struct cfg_directive {
    const char *key;
    const char *line;
    bool replaced;
};

/* The directive in @p dirs whose key matches @p line's first token, or NULL. */
static struct cfg_directive *match_directive(struct cfg_directive *dirs,
                                             size_t n, const char *line)
{
    for (size_t i = 0; i < n; i++) {
        if (line_has_key(line, dirs[i].key)) {
            return &dirs[i];
        }
    }
    return NULL;
}

/* Copy @p in to @p out line by line.  Each directive in @p dirs is replaced in
 * place by its line, preserving its position next to any associated comment;
 * duplicate occurrences are dropped.  Directives not already present are
 * appended at the end, in array order. */
static int rewrite_directives(struct fs_file_t *in, struct fs_file_t *out,
                              struct cfg_directive *dirs, size_t n)
{
    char buf[LINE_MAX];
    int pos = 0;
    ssize_t nread;
    int ret;

    while ((nread = fs_read(in, buf + pos, 1)) == 1) {
        if (buf[pos] == '\n') {
            buf[pos + 1] = '\0';
            struct cfg_directive *d = match_directive(dirs, n, buf);
            if (d) {
                if (!d->replaced) {
                    ret = write_all(out, d->line, strlen(d->line));
                    if (ret) {
                        return ret;
                    }
                    d->replaced = true;
                }
                /* else: a duplicate of an already-replaced key — drop it. */
            } else {
                ret = write_all(out, buf, pos + 1);
                if (ret) {
                    return ret;
                }
            }
            pos = 0;
        } else if (pos >= LINE_MAX - 2) {
            LOG_ERR("Config line too long (max %d chars)", LINE_MAX - 2);
            return -EINVAL;
        } else {
            pos++;
        }
    }

    if (nread < 0) {
        LOG_ERR("Read error in %s: %d", CONFIG_PATH, (int)nread);
        return (int)nread;
    }

    /* A final line with no trailing newline. */
    if (pos > 0) {
        buf[pos] = '\0';
        struct cfg_directive *d = match_directive(dirs, n, buf);
        if (d) {
            if (!d->replaced) {
                ret = write_all(out, d->line, strlen(d->line));
                if (ret) {
                    return ret;
                }
                d->replaced = true;
            }
        } else {
            /* Terminate it so an appended directive lands on its own line. */
            buf[pos] = '\n';
            ret = write_all(out, buf, pos + 1);
            if (ret) {
                return ret;
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (!dirs[i].replaced) {
            ret = write_all(out, dirs[i].line, strlen(dirs[i].line));
            if (ret) {
                return ret;
            }
        }
    }

    return 0;
}

/* Do the rewrite, assuming the caller already holds the filesystem lock. */
static int rewrite_config_file(struct cfg_directive *dirs, size_t n)
{
    struct fs_file_t in;
    struct fs_file_t out;
    fs_file_t_init(&in);
    fs_file_t_init(&out);

    int ret = fs_open(&in, CONFIG_PATH, FS_O_READ);
    if (ret) {
        LOG_ERR("Failed to open %s: %d", CONFIG_PATH, ret);
        return ret;
    }

    ret = fs_open(&out, CONFIG_TMP_PATH, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (ret) {
        LOG_ERR("Failed to open %s: %d", CONFIG_TMP_PATH, ret);
        fs_close(&in);
        return ret;
    }

    ret = rewrite_directives(&in, &out, dirs, n);

    fs_close(&in);
    fs_close(&out);

    if (ret) {
        fs_unlink(CONFIG_TMP_PATH);
        return ret;
    }

    /* FATfs rename fails if the target exists, so drop the original first.
     * The gap between unlink and rename is the only point a crash could lose
     * the config — microseconds, on a hand-run shell command. */
    ret = fs_unlink(CONFIG_PATH);
    if (ret) {
        LOG_ERR("Failed to remove old %s: %d", CONFIG_PATH, ret);
        fs_unlink(CONFIG_TMP_PATH);
        return ret;
    }

    ret = fs_rename(CONFIG_TMP_PATH, CONFIG_PATH);
    if (ret) {
        LOG_ERR("Failed to rename %s to %s: %d", CONFIG_TMP_PATH,
                CONFIG_PATH, ret);
        return ret;
    }

    return 0;
}

/* Rewrite /SD:/bleatbox.cfg, replacing each directive in @p dirs in place (or
 * appending it if absent) and leaving every other line untouched.  Writing all
 * directives in one pass keeps related values (e.g. delay_min/delay_max)
 * consistent on disk even if a later one fails.
 *
 * Holds the filesystem lock across the whole rewrite so it cannot race audio
 * playback reading the SD card on another thread. */
static int save_directives(struct cfg_directive *dirs, size_t n)
{
    if (!sdcard_is_mounted()) {
        return -ENODEV;
    }

    sdcard_lock();
    int ret = rewrite_config_file(dirs, n);
    sdcard_unlock();

    return ret;
}

int device_config_save_id(uint8_t id)
{
    char line[16];
    snprintf(line, sizeof(line), "id %02X\n", id);

    struct cfg_directive d = {.key = "id", .line = line};
    return save_directives(&d, 1);
}

int device_config_save_volume(uint8_t volume)
{
    char line[16];
    snprintf(line, sizeof(line), "volume %u\n", volume);

    struct cfg_directive d = {.key = "volume", .line = line};
    return save_directives(&d, 1);
}

int device_config_save_delay(uint16_t delay_min_ms, uint16_t delay_max_ms)
{
    char min_line[24];
    char max_line[24];
    snprintf(min_line, sizeof(min_line), "delay_min %u\n", delay_min_ms);
    snprintf(max_line, sizeof(max_line), "delay_max %u\n", delay_max_ms);

    struct cfg_directive dirs[] = {
        {.key = "delay_min", .line = min_line},
        {.key = "delay_max", .line = max_line},
    };
    return save_directives(dirs, ARRAY_SIZE(dirs));
}

int device_config_save_accel_threshold(uint16_t threshold_mg)
{
    char line[32];
    snprintf(line, sizeof(line), "accel_threshold %u\n", threshold_mg);

    struct cfg_directive d = {.key = "accel_threshold", .line = line};
    return save_directives(&d, 1);
}

int device_config_save_relay_ttl(uint8_t relay_ttl)
{
    char line[16];
    snprintf(line, sizeof(line), "relay_ttl %u\n", relay_ttl);

    struct cfg_directive d = {.key = "relay_ttl", .line = line};
    return save_directives(&d, 1);
}
