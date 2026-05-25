/*
 * Audio subsystem — SD card mounting and sound playback.
 */

#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdint.h>

/**
 * Mount the FAT32-formatted SD card.
 *
 * @return 0 on success, negative errno on failure.
 */
int audio_sd_mount(void);

/**
 * Play an MP3 file from the SD card through the VS1053B.
 *
 * Acquires the playback mutex for the duration of playback.
 * Checks the cancel flag between 32-byte chunks — call
 * audio_cancel_playback() from another thread to preempt.
 *
 * @param sound_index  File to play (maps to "/<index>.mp3").
 */
void audio_play_sound(uint8_t sound_index);

/**
 * Request cancellation of any in-progress playback.
 *
 * The play_sound loop checks this flag between chunks and exits
 * early, releasing the playback mutex.  Safe to call from ISR
 * context or any thread.
 */
void audio_cancel_playback(void);

#endif /* AUDIO_H_ */
