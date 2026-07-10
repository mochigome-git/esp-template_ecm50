#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
#include "modbus_utils.h"

/* Generic function-pointer shapes. Every decode_<device>.c module exposes
 * a pair of *_generic() wrappers matching these (see decode_powermeter.c
 * for the pattern — they just cast void* to the module's own typed
 * struct and forward to the module's real, typed functions). */
typedef esp_err_t (*modbus_device_read_fn_t)(modbus_t *mb, uint8_t slave_addr, void *out);
typedef cJSON    *(*modbus_device_json_fn_t)(const void *data);

/* Optional side-effect hook, e.g. driving a relay/alarm output off a
 * threshold. Called with the decoded data right after a successful
 * read, before the payload is built. Leave NULL for devices that don't
 * need one (see decode_powermeter.c's table entry — it has none). */
typedef void (*modbus_device_hook_fn_t)(const void *data);

/* Largest decoded-data struct any device module produces. main.c uses a
 * stack buffer this size as scratch space for whichever device it's
 * currently polling. Bump this if a future decode_*.c's struct grows
 * past it — modbus_devices.c has a _Static_assert that will catch it if
 * you forget. */
#define MODBUS_DEVICE_MAX_DATA_SIZE 128

/* One row of the device table in modbus_devices.c. Purely about how to
 * poll a Modbus slave — telemetry identity (tenant_id, device_id,
 * machine_id, lot_id) is gateway-wide and comes from config.h, the same
 * for every payload regardless of which slave the readings came from. */
typedef struct {
    const char *name;         /* label for log lines only, e.g. "power-meter-1" */
    const char *payload_field; /* which top-level payload key to put this
                                 * device's decoded data in: "readings",
                                 * "energy", "output", "limits", "metric_a",
                                 * "metric_b", or "metric_c" — must match one
                                 * of the null placeholders build_payload()
                                 * creates in main.c. */
    uint8_t     slave_addr;   /* Modbus slave address (1-127) */
    size_t      data_size;    /* sizeof(<device>_data_t); must be <= MODBUS_DEVICE_MAX_DATA_SIZE */
    modbus_device_read_fn_t read;     /* reads + decodes registers into `out` */
    modbus_device_json_fn_t to_json;  /* builds cJSON from decoded data (data->ok assumed true) */
    modbus_device_hook_fn_t on_data;  /* optional; NULL if this device needs no side effect */
} modbus_device_t;