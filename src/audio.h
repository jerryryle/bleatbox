/*
 * Audio subsystem — SD card mounting and sound playback.
 */

#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdbool.h>
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
 * Plays the entire file to completion.  Callers should check
 * audio_is_playing() first and drop the event if a sound is
 * already in progress — this function does not preempt.
 *
 * @param sound_index  File to play (maps to "/<index>.mp3").
 */
void audio_play_sound(uint8_t sound_index);

/**
 * Check whether a sound is currently playing.
 *
 * Safe to call from any context including ISRs.
 *
 * @return true if playback is in progress, false otherwise.
 */
bool audio_is_playing(void);

#endif /* AUDIO_H_ */
