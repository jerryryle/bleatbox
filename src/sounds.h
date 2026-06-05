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

/**
 * Get the full filesystem path for a sound by index.
 *
 * @param index  Sound index (0-based).
 * @param buf    Buffer to write the path into.
 * @param len    Size of @p buf in bytes.
 * @return 0 on success, -EINVAL if index is out of range,
 *         -ENOSPC if the buffer is too small.
 */
int sounds_get_path(uint8_t index, char *buf, size_t len);

#endif /* SOUNDS_H_ */
