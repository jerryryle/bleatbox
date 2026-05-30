/*
 * Audio subsystem — sound playback via VS1053B codec.
 */

#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize the audio subsystem (SPI bus and VS1053B codec).
 *
 * Must be called before audio_play_sound() or audio_set_volume().
 *
 * @return 0 on success, negative errno on failure.
 */
int audio_init(void);

/**
 * Set playback volume.
 *
 * @param percent  0 (silent) to 100 (maximum).
 * @return 0 on success, negative errno on failure.
 */
int audio_set_volume(uint8_t percent);

/**
 * Start playing an MP3 file from the SD card through the VS1053B.
 *
 * Returns immediately.  Playback runs on a dedicated background
 * thread.  audio_is_playing() returns true from the moment this
 * function is called until the file has been fully streamed.
 *
 * Calling this while a sound is already playing is a no-op (the
 * call is logged and ignored).
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
