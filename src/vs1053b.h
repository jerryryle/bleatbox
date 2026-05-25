/*
 * VS1053B codec driver — SPI command/data interface.
 */

#ifndef VS1053B_H_
#define VS1053B_H_

#include <stdint.h>
#include <stddef.h>

/**
 * Initialize the VS1053B: configure DREQ GPIO, hardware reset,
 * boost internal clock, and set default volume.
 *
 * @param spi_dev  The SPI device shared with the SD card.
 * @return 0 on success, negative errno on failure.
 */
int vs1053b_init(const struct device *spi_dev);

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

/** Size of one SDI data chunk (bytes). */
#define VS1053B_DATA_CHUNK 32

#endif /* VS1053B_H_ */
