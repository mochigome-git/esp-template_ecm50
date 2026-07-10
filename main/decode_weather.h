#pragma once
#include <stdint.h>
#include "cJSON.h"

/* Weather decoded channels — mirrors CHANNEL_KEYS in decode_weather.py */
typedef struct {
    float    wind_speed;       /* m/s  */
    char     wind_direction[4];/* "N","NE",… */
    uint16_t wind_angle;       /* degrees  */
    float    humidity;         /* %        */
    float    temperature;      /* °C       */
    float    noise;            /* dB       */
    uint16_t pm25;
    uint16_t pm10;
    float    pressure;         /* hPa      */
    uint32_t light;            /* lux      */
} weather_data_t;

/**
 * Decode all weather blocks.
 *
 * @param wind      4 raw int16 registers (or NULL if block read failed)
 * @param thn       3 raw int16 registers
 * @param pm        3 raw int16 registers
 * @param light     2 raw int16 registers
 * @param out       Filled on success
 * @return true if at least one block decoded successfully
 */
bool decode_weather(const int16_t *wind,
                    const int16_t *thn,
                    const int16_t *pm,
                    const int16_t *light,
                    weather_data_t *out);

/**
 * Serialise decoded weather into a cJSON object suitable for the
 * "readings" field of the MQTT payload.
 * Caller must cJSON_Delete() the returned object.
 */
cJSON *weather_to_json(const weather_data_t *d);
