#include "wrap_to_range.h"

uint32_t wrap_to_range(uint32_t value, uint32_t min, uint32_t max)
{
    if (min >= max) {
        return min;
    }
    return min + (value - min) % (max - min + 1);
}
