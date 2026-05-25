/*
 * Vibration switch — debounced GPIO event producer.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "vibration.h"
#include "events.h"

LOG_MODULE_REGISTER(vibration, LOG_LEVEL_INF);

#define DEBOUNCE_MS 200

static const struct gpio_dt_spec vibration_sw =
	GPIO_DT_SPEC_GET(DT_NODELABEL(vibration_sw), gpios);

static struct gpio_callback vibration_cb_data;
static int64_t last_trigger_time;

/* Set during init; used by the ISR to post events. */
static struct k_msgq *evt_q;

static void vibration_isr(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	int64_t now = k_uptime_get();
	if ((now - last_trigger_time) < DEBOUNCE_MS) {
		return;
	}
	last_trigger_time = now;

	struct event evt = { .type = EVENT_VIBRATION };
	k_msgq_put(evt_q, &evt, K_NO_WAIT);
}

int vibration_init(struct k_msgq *event_q)
{
	evt_q = event_q;
	int ret;

	if (!gpio_is_ready_dt(&vibration_sw)) {
		LOG_ERR("Vibration switch GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&vibration_sw, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure vibration switch: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&vibration_sw,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure vibration interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&vibration_cb_data, vibration_isr,
			   BIT(vibration_sw.pin));
	gpio_add_callback(vibration_sw.port, &vibration_cb_data);

	return 0;
}
