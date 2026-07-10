#include "modbus_devices.h"
#include "decode_powermeter.h"

/* ══════════════════════════════════════════════════════════
 *  Modbus device table
 *
 *  This is the only file you edit to add/remove/reconfigure a Modbus
 *  slave. main.c just walks this array — it never needs to change.
 *
 *  To add a new slave:
 *    1. Write decode_<device>.h/.c exposing <device>_data_t,
 *       <device>_read_generic(), <device>_to_json_generic()
 *       (see decode_powermeter.c for the pattern).
 *    2. #include "decode_<device>.h" below.
 *    3. Add a { ... } entry to MODBUS_DEVICES.
 * ══════════════════════════════════════════════════════════ */

const modbus_device_t MODBUS_DEVICES[] = {
    {
        .name       = "power-meter-1",
        .slave_addr = 1,
        .data_size  = sizeof(powermeter_data_t),
        .read       = powermeter_read_generic,
        .to_json    = powermeter_to_json_generic,
    },
    /* Next slave goes here once you share its register table, e.g.:
    {
        .name       = "weather-1",
        .slave_addr = 2,
        .data_size  = sizeof(weather_data_t),
        .read       = weather_read_generic,
        .to_json    = weather_to_json_generic,
    },
    */
};

const size_t MODBUS_DEVICE_COUNT = sizeof(MODBUS_DEVICES) / sizeof(MODBUS_DEVICES[0]);

_Static_assert(sizeof(powermeter_data_t) <= MODBUS_DEVICE_MAX_DATA_SIZE,
               "powermeter_data_t exceeds MODBUS_DEVICE_MAX_DATA_SIZE — bump it in modbus_device.h");