/*
 * Fake <zephyr/random/random.h> for host-native unit tests.
 *
 * Declares sys_rand32_get() so that production .c files compile
 * unchanged.  The test file provides the definition.
 */

#ifndef ZEPHYR_RANDOM_RANDOM_H_
#define ZEPHYR_RANDOM_RANDOM_H_

#include <stdint.h>

uint32_t sys_rand32_get(void);

#endif /* ZEPHYR_RANDOM_RANDOM_H_ */
