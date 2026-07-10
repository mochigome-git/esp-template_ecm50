#pragma once

/**
 * Device-table on_data hook (see modbus_device_t) for the pressure
 * sensor. Called with a `const pressure_data_t *` right after a
 * successful read. Turns the DO1/DO2 blink pattern on (relay_alarm_set)
 * when pressure has dropped to or below PRESSURE_ALARM_THRESHOLD_MPA
 * (config.h), off otherwise.
 */
void pressure_alarm_check(const void *data);