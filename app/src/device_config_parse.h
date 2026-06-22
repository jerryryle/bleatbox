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
 * Returned by device_config_parse_line() when the line's directive keyword is
 * not recognized.  The line is otherwise well-formed and is ignored; the
 * caller may warn.  Distinct from 0 (handled) and negative errno (malformed).
 */
#define DEVICE_CONFIG_UNKNOWN_KEY 1

/**
 * Parse a single config line and populate the corresponding field
 * in @p cfg.
 *
 * The line is modified in place (strtok_r).  Comments and blank lines are
 * accepted as no-ops.
 *
 * @param line    Mutable, null-terminated config line.
 * @param cfg     Config struct to populate.
 * @param[out] has_id  Set to true if an "id" line was parsed.
 * @return 0 if handled, DEVICE_CONFIG_UNKNOWN_KEY for an unrecognized
 *         directive, or negative errno on a malformed value.
 */
int device_config_parse_line(char *line, struct device_config *cfg,
                             bool *has_id);

#endif /* DEVICE_CONFIG_PARSE_H_ */
