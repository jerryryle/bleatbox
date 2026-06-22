#ifndef SDCARD_H_
#define SDCARD_H_

#include <stdbool.h>

/** Mount point for the FAT32-formatted SD card. */
#define SDCARD_MOUNT_POINT "/SD:"

/**
 * Initialize the SD card module's internal state.
 *
 * Must be called before sdcard_mount().  Touches no hardware.
 */
void sdcard_init(void);

/**
 * Mount the SD card filesystem.
 *
 * @return 0 on success, negative errno on failure.
 */
int sdcard_mount(void);

/**
 * Check whether the SD card is mounted.
 *
 * @return true if the SD card was successfully mounted.
 */
bool sdcard_is_mounted(void);

#endif /* SDCARD_H_ */
