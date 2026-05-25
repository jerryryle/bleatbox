/*
 * Event types shared between producer modules and the main loop.
 */

#ifndef EVENTS_H_
#define EVENTS_H_

#include <stdint.h>

enum event_type {
	EVENT_VIBRATION,   /* Debounced vibration detected                */
	EVENT_BLE_RX,      /* BLE broadcast with our assignment received  */
};

struct event {
	enum event_type type;
	uint8_t  sound;      /* EVENT_BLE_RX: assigned sound index       */
	uint16_t delay_ms;   /* EVENT_BLE_RX: assigned delay before play */
};

#endif /* EVENTS_H_ */
