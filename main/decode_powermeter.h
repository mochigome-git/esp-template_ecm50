#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
#include "modbus_utils.h"

/* 三相交流采集器 (南京杰效) — combined-phase ("合相") electrical data,
 * matches the target payload shape:
 * {
 *   "v_rms": 220.5, "i_rms": 3.21,
 *   "p_active_kw": 0.65, "p_reactive_kvar": 0.12, "p_apparent_kva": 0.68,
 *   "power_factor": 0.95, "frequency_hz": 50.0,
 *   "energy_import_kwh": 12345.6, "energy_export_kwh": 0.0,
 *   "energy_active_total_kwh": 12345.6,
 *   "energy_reactive_import_kvarh": 0.0, "energy_reactive_export_kvarh": 0.0,
 *   "energy_reactive_total_kvarh": 0.0
 * }
 */
typedef struct {
    double v_rms;
    double i_rms;
    double p_active_kw;
    double p_reactive_kvar;
    double p_apparent_kva;
    double power_factor;
    double frequency_hz;
    double energy_import_kwh;
    double energy_export_kwh;
    double energy_active_total_kwh;
    double energy_reactive_import_kvarh;
    double energy_reactive_export_kvarh;
    double energy_reactive_total_kvarh;
    bool   ok;   /* true if the Modbus read + parse succeeded */
} powermeter_data_t;

/**
 * Read all fields for one meter from its "合相" holding registers
 * (0x1000-0x106B, 108 registers) in a single Modbus request, and decode
 * them directly into `out`. Combines what read_weather_blocks() +
 * decode_weather() used to do separately, since this meter's fields all
 * live in one contiguous block.
 *
 * @param mb          initialised modbus_t handle (RTU or TCP)
 * @param slave_addr  Modbus slave address (1-127)
 * @param out         filled with decoded values on success
 */
esp_err_t powermeter_read(modbus_t *mb, uint8_t slave_addr, powermeter_data_t *out);

/** Build a cJSON object matching the target payload shape. Returns NULL if !out->ok.
 *  Caller owns the returned object and must cJSON_Delete() it. */
cJSON *powermeter_to_json(const powermeter_data_t *out);

/* ── Generic wrappers for the modbus_device_t table (modbus_devices.c) ──
 * Just cast void* to powermeter_data_t* and forward. Every decode_*.c
 * module needs a matching pair of these to be pluggable into the table. */
esp_err_t powermeter_read_generic(modbus_t *mb, uint8_t slave_addr, void *out);
cJSON    *powermeter_to_json_generic(const void *data);