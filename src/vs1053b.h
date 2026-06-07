/*
 * VS1053B codec driver — SPI command/data interface.
 */

#ifndef VS1053B_H_
#define VS1053B_H_

#include <zephyr/device.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Initialize the VS1053B GPIOs and hold the codec in hardware reset.
 *
 * Does not configure the codec — it stays at ~12 uA until the first
 * vs1053b_power_up(), which releases reset and programs it.  The
 * default volume is cached and applied on that first power-up.
 *
 * @param spi_dev  The SPI device shared with the SD card.
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_init(const struct device *spi_dev);

/**
 * Set playback volume on both channels.
 *
 * @param percent  0 (silent) to 100 (maximum).
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_get_volume(uint8_t *percent);

int vs1053b_set_volume(uint8_t percent);

/**
 * Write a chunk of audio data to the VS1053B SDI (data) interface.
 *
 * Waits for DREQ before transmitting.  Caller is responsible for
 * holding the playback mutex if concurrent access is possible.
 *
 * @param data  Pointer to audio data (typically 32 bytes).
 * @param len   Number of bytes to send.
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_write_data(const uint8_t *data, size_t len);

/**
 * Write a 16-bit value to an SCI register.
 *
 * Waits for DREQ before writing.
 *
 * @param reg  Register address (0x00–0x0F).
 * @param val  16-bit value to write.
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_write_reg(uint8_t reg, uint16_t val);

/**
 * Start/stop the VS1053B's internal sine test tone.
 *
 * @param enable  true to start, false to stop.
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_sine_test(bool enable);

/**
 * Enter low-power mode: mute the analog outputs, then assert the
 * codec's hardware reset, dropping it to ~12 uA.  All SCI register
 * state is lost; vs1053b_power_up() reconfigures the codec.
 *
 * Requires the FeatherWing RST jumper cut and wired to the reset GPIO.
 * On an unmodified board the pin is unconnected and the codec stays in
 * its clocked idle state.
 *
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_power_down(void);

/**
 * Exit low-power mode: release the hardware reset and fully reconfigure
 * the codec (clock boost and cached volume), since reset cleared all
 * registers.  Takes a few ms for the oscillator to start.
 *
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_power_up(void);

/**
 * Finish playback and reset the codec for the next file.
 *
 * Must be called after the last audio data is sent.  Sends endFillByte
 * padding, then performs a soft reset so the decoder is ready to accept
 * a new stream.
 *
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_end_playback(void);

/** Size of one SDI data chunk (bytes). */
#define VS1053B_DATA_CHUNK 32

#endif /* VS1053B_H_ */
