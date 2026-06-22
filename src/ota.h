#ifndef OTA_H_
#define OTA_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/*
 * OTA update window — main-app side of the over-the-air update flow.
 *
 * A BLE OTA-arm command opens a window during which the box is connectable
 * for SMP-over-BLE, so an operator can upload a new firmware image onto the
 * SD card (`smpmgr file upload`) and then reboot the box (`smpmgr os reset`);
 * the second-stage updater applies the staged image on the next boot.
 *
 * Two-phase init: ota_init() wires state with no side effects;
 * ota_set_device_id() supplies the config-derived id used in the
 * connectable advertising name.  The window itself opens only in response
 * to an arm command, and closes on a timeout (or ota_cancel()).
 */

/* Phase 1: initialize state.  Must run after ble_init() (BT enabled). */
int ota_init(struct k_msgq *event_q);

/* Phase 2: provide the device id used in the "bleatbox-dfu-<id>" name. */
void ota_set_device_id(uint8_t device_id);

/* Open the update window (become connectable for SMP). */
void ota_start(void);

/* Close the update window and stop connectable advertising. */
void ota_cancel(void);

/* True while an update window is open (gates audio triggers). */
bool ota_is_active(void);

#endif /* OTA_H_ */
