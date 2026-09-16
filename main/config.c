#include "config.h"
#include "decode_powermeter.h"
#include "decode_pressure.h"
#include "pressure_alarm.h"
#include "machine_run.h"

/* ══════════════════════════════════════════════════════════
 *  Modbus device table
 *
 *  This is the only file you edit to add/remove/reconfigure a Modbus
 *  slave (declared as `extern` in config.h). main.c just walks
 *  MODBUS_DEVICES[] — it never needs to change.
 *
 *  To add a new slave:
 *    1. Write decode_<device>.h/.c exposing <device>_data_t,
 *       <device>_read_generic(), <device>_to_json_generic()
 *       (see decode_powermeter.c for the pattern).
 *    2. #include "decode_<device>.h" below.
 *    3. Add a { ... } entry to MODBUS_DEVICES.
 * ══════════════════════════════════════════════════════════ */

/* Payload field option
- metric_a  text
- metric_b  text
- metric_c  text
- readings  jsonb
- output    jsonb
- limits    jsonb
- energy    jsonb
- status    jsonb
*/

const modbus_device_t MODBUS_DEVICES[] = {
    {
        .name = "power-meter",
        .payload_field = "energy",
        .slave_addr = 1,
        .data_size = sizeof(powermeter_data_t),
        .read = powermeter_read_generic,
        .to_json = powermeter_to_json_generic,
        .on_data = machine_run_capture, /* caches i_rms for the pressure-alarm run gate */
        .publish_interval_s = 10,
    },
    {
        .name = "power-meter",
        .payload_field = "energy",
        .slave_addr = 2,
        .data_size = sizeof(powermeter_data_t),
        .read = powermeter_read_generic,
        .to_json = powermeter_to_json_generic,
        .on_data = machine_run_capture, /* caches i_rms for the pressure-alarm run gate */
        .publish_interval_s = 10,
    },

    /*
    {
        .name = "pressure",
        .payload_field = "readings",
        .slave_addr = 2,
        .data_size = sizeof(pressure_data_t),
        .read = pressure_read_generic,
        .to_json = pressure_to_json_generic,
        .on_data = pressure_alarm_check,  // drives DO1/DO2 off the runtime threshold
        .publish_interval_s = 30,
    },
    */

    /* Next slave goes here once you share its register table, e.g.:
    {
        .name          = "weather-1",
        .payload_field = "readings",
        .slave_addr    = 2,
        .data_size     = sizeof(weather_data_t),
        .read          = weather_read_generic,
        .to_json       = weather_to_json_generic,
    },
    */
};

const size_t MODBUS_DEVICE_COUNT = sizeof(MODBUS_DEVICES) / sizeof(MODBUS_DEVICES[0]);

_Static_assert(sizeof(powermeter_data_t) <= MODBUS_DEVICE_MAX_DATA_SIZE,
               "powermeter_data_t exceeds MODBUS_DEVICE_MAX_DATA_SIZE — bump it in modbus_device.h");
_Static_assert(sizeof(pressure_data_t) <= MODBUS_DEVICE_MAX_DATA_SIZE,
               "pressure_data_t exceeds MODBUS_DEVICE_MAX_DATA_SIZE — bump it in modbus_device.h");