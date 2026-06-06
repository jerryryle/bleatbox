#ifndef DEVICE_CONFIG_PARSE_H_
#define DEVICE_CONFIG_PARSE_H_

#include <stdbool.h>
#include <stdint.h>

#include "device_config.h"

/**
 * Parse a hex byte string (e.g. "FF", "0a") into a uint8_t.
 *
 * @param s    Null-terminated hex string.
 * @param[out] out  Parsed byte value.
 * @return 0 on success, -EINVAL on bad format or value > 0xFF.
 */
int device_config_parse_hex_byte(const char *s, uint8_t *out);

/**
 * Parse a single config line and populate the corresponding field
 * in @p cfg.
 *
 * The line is modified in place (strtok_r).  Comments, blank lines,
 * and unknown keys are silently accepted.
 *
 * @param line    Mutable, null-terminated config line.
 * @param cfg     Config struct to populate.
 * @param[out] has_id  Set to true if an "id" line was parsed.
 * @return 0 on success, negative errno on parse error.
 */
int device_config_parse_line(char *line, struct device_config *cfg,
                             bool *has_id);

#endif /* DEVICE_CONFIG_PARSE_H_ */
