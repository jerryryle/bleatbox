#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "device_config_parse.h"

int device_config_parse_hex_byte(const char *s, uint8_t *out)
{
    char *end;
    unsigned long val = strtoul(s, &end, 16);

    if (end == s || *end != '\0' || val > 0xFF) {
        return -EINVAL;
    }
    *out = (uint8_t)val;
    return 0;
}

/*
 * Parse a full decimal token into [0, max].  Trailing junk is
 * rejected — "100abc" must not silently parse as 100.
 */
static int parse_dec(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long val = strtoul(s, &end, 10);

    if (end == s || *end != '\0' || val > max) {
        return -EINVAL;
    }
    *out = val;
    return 0;
}

int device_config_parse_line(char *line, struct device_config *cfg,
                             bool *has_id)
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
            return -EINVAL;
        }
        if (device_config_parse_hex_byte(val, &cfg->id)) {
            return -EINVAL;
        }
        *has_id = true;
    } else if (strcmp(key, "volume") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        unsigned long v;
        if (!val || parse_dec(val, 100, &v)) {
            return -EINVAL;
        }
        cfg->volume = (uint8_t)v;
    } else if (strcmp(key, "delay_min") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        unsigned long v;
        if (!val || parse_dec(val, UINT16_MAX, &v)) {
            return -EINVAL;
        }
        cfg->delay_min_ms = (uint16_t)v;
    } else if (strcmp(key, "delay_max") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        unsigned long v;
        if (!val || parse_dec(val, UINT16_MAX, &v)) {
            return -EINVAL;
        }
        cfg->delay_max_ms = (uint16_t)v;
    } else if (strcmp(key, "accel_threshold") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        unsigned long v;
        if (!val || parse_dec(val, UINT16_MAX, &v) || v == 0) {
            return -EINVAL;
        }
        cfg->accel_threshold_mg = (uint16_t)v;
    } else if (strcmp(key, "relay_ttl") == 0) {
        char *val = strtok_r(NULL, " \t\r\n", &saveptr);
        unsigned long v;
        if (!val || parse_dec(val, 255, &v)) {
            return -EINVAL;
        }
        cfg->relay_ttl = (uint8_t)v;
    } else {
        return DEVICE_CONFIG_UNKNOWN_KEY;
    }

    return 0;
}
