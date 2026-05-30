/*
 * Audio subsystem — SD card mounting and sound playback.
 *
 * Playback runs on a dedicated background thread.  audio_play_sound()
 * signals the thread and returns immediately so the caller is never
 * blocked during audio streaming.
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
/* Playback thread                                                    */
/* ------------------------------------------------------------------ */

/*
 * True from the moment audio_play_sound() is called until the
 * background thread finishes streaming.  Set by the caller before
 * signaling the thread (no race window).  Read from ISR / scan
 * callback / main loop to gate event processing.
 */
static volatile bool playing;

static uint8_t pending_sound;
static K_SEM_DEFINE(play_sem, 0, 1);

static void audio_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sem_take(&play_sem, K_FOREVER);

		uint8_t sound_index = pending_sound;

		if (!sd_mounted) {
			LOG_ERR("SD not mounted, cannot play sound %u",
				sound_index);
			playing = false;
			continue;
		}

		char path[32];
		snprintf(path, sizeof(path), "/SD:/%u.mp3", sound_index);

		struct fs_file_t f;
		fs_file_t_init(&f);
		int ret = fs_open(&f, path, FS_O_READ);
		if (ret < 0) {
			LOG_ERR("Cannot open %s: %d", path, ret);
			playing = false;
			continue;
		}

		LOG_INF("Playing %s", path);

		uint8_t buf[VS1053B_DATA_CHUNK];
		ssize_t nread;
		while ((nread = fs_read(&f, buf, sizeof(buf))) > 0) {
			ret = vs1053b_write_data(buf, nread);
			if (ret) {
				LOG_ERR("Playback aborted: SPI error");
				break;
			}
		}

		fs_close(&f);
		playing = false;
		LOG_INF("Playback finished");
	}
}

#define AUDIO_STACK_SIZE 4096
#define AUDIO_PRIORITY   5

K_THREAD_DEFINE(audio_tid, AUDIO_STACK_SIZE,
		audio_thread_entry, NULL, NULL, NULL,
		AUDIO_PRIORITY, 0, 0);

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool audio_is_sd_mounted(void)
{
	return sd_mounted;
}

bool audio_is_playing(void)
{
	return playing;
}

void audio_play_sound(uint8_t sound_index)
{
	if (playing) {
		LOG_WRN("audio_play_sound called while already playing — ignored");
		return;
	}

	pending_sound = sound_index;
	playing = true;
	k_sem_give(&play_sem);
}
