#ifndef WRAP_TO_RANGE_H_
#define WRAP_TO_RANGE_H_

#include <stdint.h>

/** Wrap @p value into [min, max] inclusive via modular arithmetic. */
uint32_t wrap_to_range(uint32_t value, uint32_t min, uint32_t max);

#endif /* WRAP_TO_RANGE_H_ */
