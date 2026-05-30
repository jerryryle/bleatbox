/*
 * Audio subsystem — sound playback via VS1053B codec.
 */

#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

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
