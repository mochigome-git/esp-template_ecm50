#pragma once
#include <stdbool.h>
#include "esp_err.h"

/**
 * Configure DO1/DO2 (RELAY_PIN_DO1/RELAY_PIN_DO2 in config.h) as GPIO
 * outputs. Call once at boot. Both outputs start OPEN/low.
 */
esp_err_t relay_control_init(void);

/**
 * Drive DO1 and DO2 together, latched steady (no blinking/pulsing).
 * @param active  true = both energized/on, false = both open/off.
 */
void relay_alarm_set(bool active);