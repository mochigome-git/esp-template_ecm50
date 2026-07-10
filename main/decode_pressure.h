#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
#include "modbus_utils.h"

/* Pressure transducer — reads the decimal-point config register (0003H)
 * and the raw pressure value (0004H) in one request, then scales:
 *
 *   pressure = raw_value / 10^decimals
 *
 * Unit depends on how register 0002H ("压力单位") was configured on the
 * device (MPa/kPa/Pa/Bar/mBar/kg per cm2/psi/mH2O/mmH2O) — that register
 * isn't read here, so the resulting number's unit is whatever the sensor
 * was provisioned with, not something this code can label automatically. */
typedef struct {
    double pressure;
    bool   ok;
} pressure_data_t;

/**
 * Read the decimal-point + raw pressure registers and decode the
 * scaled pressure value.
 *
 * @param mb          initialised modbus_t handle (RTU or TCP)
 * @param slave_addr  Modbus slave address (1-254)
 * @param out         filled with decoded value on success
 */
esp_err_t pressure_read(modbus_t *mb, uint8_t slave_addr, pressure_data_t *out);

/** Build a cJSON object: { "pressure": <value> }. Returns NULL if !out->ok.
 *  Caller owns the returned object and must cJSON_Delete() it. */
cJSON *pressure_to_json(const pressure_data_t *out);

/* ── Generic wrappers for the modbus_device_t table (config.c) ── */
esp_err_t pressure_read_generic(modbus_t *mb, uint8_t slave_addr, void *out);
cJSON    *pressure_to_json_generic(const void *data);