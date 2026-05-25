/*
 * Audio subsystem — SD card mounting and sound playback.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>

#include <stdio.h>

#include "audio.h"
#include "vs1053b.h"

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* SD card and filesystem                                             */
/* ------------------------------------------------------------------ */

static FATFS fat_fs;
static struct fs_mount_t mount_point = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};
static bool sd_mounted;

int audio_sd_mount(void)
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

	sd_mounted = true;
	LOG_INF("SD card mounted at %s", mount_point.mnt_point);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Playback                                                           */
/* ------------------------------------------------------------------ */

/*
 * True while audio_play_sound is streaming data to the VS1053B.
 * Read by the vibration ISR and BLE scan callback to drop events
 * during playback.  Volatile because it is written by one thread
 * and read from ISR / other thread contexts.
 */
static volatile bool playing;

bool audio_is_playing(void)
{
	return playing;
}

void audio_play_sound(uint8_t sound_index)
{
	if (!sd_mounted) {
		LOG_ERR("SD not mounted, cannot play sound %u", sound_index);
		return;
	}

	char path[32];
	snprintf(path, sizeof(path), "/SD:/%u.mp3", sound_index);

	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("Cannot open %s: %d", path, ret);
		return;
	}

	LOG_INF("Playing %s", path);
	playing = true;

	uint8_t buf[VS1053B_DATA_CHUNK];
	ssize_t nread;
	while ((nread = fs_read(&f, buf, sizeof(buf))) > 0) {
		ret = vs1053b_write_data(buf, nread);
		if (ret) {
			LOG_ERR("Playback aborted: SPI error");
			break;
		}
	}

	playing = false;
	fs_close(&f);
	LOG_INF("Playback finished");
}
