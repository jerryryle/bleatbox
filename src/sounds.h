#ifndef SOUNDS_H_
#define SOUNDS_H_

#include <stdint.h>
#include <stddef.h>

enum sound_type {
    SOUND_TYPE_GOAT,
    SOUND_TYPE_MISC,
};

/**
 * Scan the SD card for numbered .mp3 files and record the counts.
 *
 * Scans for goat and misc prefixes. Goat sounds must be present;
 * misc sounds are optional.
 *
 * @return 0 on success, negative errno on failure.
 */
int sounds_scan(void);

/**
 * Number of available sounds for a given type.
 *
 * @return count, or 0 if scan not yet run or nothing found.
 */
uint8_t sounds_get_count(enum sound_type type);

/**
 * Get the full filesystem path for a sound.
 *
 * @param type   Sound type (goat or misc).
 * @param index  Sound index (0-based).
 * @param buf    Buffer to write the path into.
 * @param len    Size of @p buf in bytes.
 * @return 0 on success, -EINVAL if index is out of range,
 *         -ENOSPC if the buffer is too small.
 */
int sounds_get_path(enum sound_type type, uint8_t index,
                    char *buf, size_t len);

#endif /* SOUNDS_H_ */
