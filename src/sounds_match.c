#include <stdlib.h>
#include <string.h>

#include "sounds_match.h"

int sounds_try_match(const char *filename, const char *dot,
                     const char *prefix, size_t prefix_len)
{
    if (strncmp(filename, prefix, prefix_len) != 0) {
        return -1;
    }

    const char *num_start = filename + prefix_len;
    size_t num_len = dot - num_start;
    if (num_len == 0 || num_len > 2) {
        return -1;
    }

    for (size_t i = 0; i < num_len; i++) {
        if (num_start[i] < '0' || num_start[i] > '9') {
            return -1;
        }
    }

    char num_str[3] = {0};
    memcpy(num_str, num_start, num_len);
    int idx = (int)strtoul(num_str, NULL, 10);
    if (idx > 99) {
        return -1;
    }

    return idx;
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
