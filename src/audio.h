/*
 * Audio subsystem — sound playback via VS1053B codec.
 */

#ifndef AUDIO_H_
#define AUDIO_H_

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize the audio subsystem (SPI bus and VS1053B codec).
 *
 * Must be called before audio_play_sound() or audio_set_volume().
 *
 * @param event_q  Message queue for posting EVENT_AUDIO_DONE when
 *                 playback finishes.  May be NULL to disable.
 * @return 0 on success, negative errno on failure.
 */
int audio_init(struct k_msgq *event_q);

/**
 * Apply a VS1053B patch from the SD card.
 *
 * The file is a flat array of big-endian uint16_t words using
 * the VLSI addr/count format (MSB of count = RLE flag).
 * Must be called after audio_init() and sdcard_mount().
 *
 * @param path  Filesystem path to the .bin patch file.
 * @return 0 on success, negative errno on failure.
 */
int audio_apply_patch(const char *path);

/**
 * Set playback volume.
 *
 * @param percent  0 (silent) to 100 (maximum).
 * @return 0 on success, negative errno on failure.
 */
int audio_get_volume(uint8_t *percent);

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
 * @param path      Filesystem path to the MP3 file.
 * @param delay_ms  Milliseconds to wait before playback starts.
 *                  The delay runs on the audio thread; audio_is_playing()
 *                  returns true for the entire duration.
 */
void audio_play_sound(const char *path, uint16_t delay_ms);

/**
 * Check whether a sound is currently playing.
 *
 * Safe to call from any context including ISRs.
 *
 * @return true if playback is in progress, false otherwise.
 */
bool audio_is_playing(void);

#endif /* AUDIO_H_ */
