#ifndef SOUNDS_H_
#define SOUNDS_H_

#include <stdint.h>

/**
 * Scan the SD card for numbered .mp3 files and record the count.
 *
 * @return 0 on success, negative errno on directory-open failure.
 */
int sounds_scan(void);

/** Number of available sounds (0 if scan not yet run or nothing found). */
uint8_t sounds_get_count(void);

#endif /* SOUNDS_H_ */
