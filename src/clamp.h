#ifndef CLAMP_H_
#define CLAMP_H_

#include <stdint.h>

/** Clamp @p value to [min, max] inclusive. */
uint32_t clamp(uint32_t value, uint32_t min, uint32_t max);

#endif /* CLAMP_H_ */
