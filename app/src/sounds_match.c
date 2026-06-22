#include <string.h>

#include "sounds_match.h"

int sounds_try_match(const char *filename, const char *dot,
                     const char *prefix, size_t prefix_len)
{
    if (strncmp(filename, prefix, prefix_len) != 0) {
        return -1;
    }

    /*
     * Exactly two digits, zero-padded — sounds_get_path() generates
     * "%02u" names, so "goat5.mp3" must not match (the generated path
     * "goat05.mp3" would not exist).
     */
    const char *num_start = filename + prefix_len;
    if (dot - num_start != 2) {
        return -1;
    }

    for (size_t i = 0; i < 2; i++) {
        if (num_start[i] < '0' || num_start[i] > '9') {
            return -1;
        }
    }

    return (num_start[0] - '0') * 10 + (num_start[1] - '0');
}

uint8_t sounds_validate_set(const bool *present, int max_index)
{
    if (max_index < 0) {
        return 0;
    }

    for (int i = 0; i <= max_index; i++) {
        if (!present[i]) {
            return 0;
        }
    }

    return (uint8_t)(max_index + 1);
}
