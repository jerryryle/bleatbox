/*
 * Audio subsystem — sound playback via VS1053B codec.
 *
 * Playback runs on a dedicated background thread.  audio_play_sound()
 * signals the thread and returns immediately so the caller is never
 * blocked during audio streaming.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <stdio.h>

#include "audio.h"
#include "sdcard.h"
#include "vs1053b.h"

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

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

		if (!sdcard_is_mounted()) {
			LOG_ERR("SD not mounted, cannot play sound %u",
				sound_index);
			playing = false;
			continue;
		}

		char path[32];
		snprintf(path, sizeof(path), SDCARD_MOUNT_POINT "/%u.mp3",
			 sound_index);

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
