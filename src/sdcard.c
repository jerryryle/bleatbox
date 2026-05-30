/*
 * SD card subsystem — FAT32 filesystem mount.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>

#include "sdcard.h"

LOG_MODULE_REGISTER(sdcard, LOG_LEVEL_INF);

static FATFS fat_fs;
static struct fs_mount_t mount_point = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = SDCARD_MOUNT_POINT,
};
static bool mounted;

int sdcard_mount(void)
{
	static const char *disk = "SD";
	uint32_t block_count, block_size;

	if (disk_access_init(disk) != 0) {
		LOG_ERR("SD card init failed");
		return -EIO;
	}
	disk_access_ioctl(disk, DISK_IOCTL_GET_SECTOR_COUNT, &block_count);
	disk_access_ioctl(disk, DISK_IOCTL_GET_SECTOR_SIZE, &block_size);
	LOG_INF("SD card: %u sectors, %u bytes/sector", block_count, block_size);

	mount_point.storage_dev = (void *)disk;
	int ret = fs_mount(&mount_point);
	if (ret != 0) {
		LOG_ERR("FAT mount failed: %d", ret);
		return ret;
	}

	mounted = true;
	LOG_INF("SD card mounted at %s", mount_point.mnt_point);
	return 0;
}

bool sdcard_is_mounted(void)
{
	return mounted;
}
