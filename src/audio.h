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
 * Get the current playback volume.
 *
 * @param[out] percent  0 (silent) to 100 (maximum).
 * @return 0 on success, negative errno on failure.
 */
int audio_get_volume(uint8_t *percent);

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
 * function returns success until the file has been fully streamed.
 *
 * The path string is copied internally; the caller's buffer does
 * not need to remain valid after this function returns.
 *
 * @param path      Filesystem path to the MP3 file (max 31 chars).
 * @param delay_ms  Milliseconds to wait before playback starts.
 *                  The delay runs on the audio thread; audio_is_playing()
 *                  returns true for the entire duration.
 * @return 0 if the request was accepted, -EBUSY if a sound is
 *         already playing (the request is ignored).
 */
int audio_play_sound(const char *path, uint16_t delay_ms);

/**
 * Start or stop the VS1053B sine test tone.
 *
 * Starting claims the codec like a playback request: trigger events
 * are ignored while the tone runs, and playback cannot start until
 * the test is stopped.  Stopping powers the codec back down and
 * posts EVENT_AUDIO_DONE so the main loop runs its vibration
 * cooldown.
 *
 * @param enable  true to start the tone, false to stop it.
 * @return 0 on success, -EBUSY if a sound is playing, -EALREADY if
 *         stopping while no test is running, or a negative errno
 *         from the codec.
 */
int audio_sine_test(bool enable);

/**
 * Check whether a sound is currently playing.
 *
 * Safe to call from any context including ISRs.
 *
 * @return true if playback is in progress, false otherwise.
 */
bool audio_is_playing(void);

#endif /* AUDIO_H_ */
