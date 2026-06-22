#include "wrap_to_range.h"

uint32_t wrap_to_range(uint32_t value, uint32_t min, uint32_t max)
{
    if (min >= max) {
        return min;
    }
    uint32_t span = max - min + 1;
    if (span == 0) {
        return value;
    }
    return min + (value - min) % span;
}
