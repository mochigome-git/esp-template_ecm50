#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "decode_weather.h"

static const char *TAG = "decode_weather";

/* ── Wind direction lookup (matches _WIND_DIR in Python) ── */
static const char *WIND_DIR[8] = {"N","NE","E","SE","S","SW","W","NW"};

/* ── Block decoders ─────────────────────────────────────── */

static bool _decode_wind(const int16_t *r, weather_data_t *out)
{
    if (!r) return false;
    out->wind_speed    = r[0] / 100.0f;
    uint16_t dir_idx   = (uint16_t)r[1];
    const char *dir    = (dir_idx < 8) ? WIND_DIR[dir_idx] : "?";
    strncpy(out->wind_direction, dir, sizeof(out->wind_direction) - 1);
    out->wind_direction[sizeof(out->wind_direction) - 1] = '\0';
    out->wind_angle    = (uint16_t)r[2];
    return true;
}

static bool _decode_thn(const int16_t *r, weather_data_t *out)
{
    if (!r) return false;
    out->humidity    = r[0] / 10.0f;
    out->temperature = r[1] / 10.0f;
    out->noise       = r[2] / 10.0f;
    return true;
}

static bool _decode_pm(const int16_t *r, weather_data_t *out)
{
    if (!r) return false;
    out->pm25     = (uint16_t)r[0];
    out->pm10     = (uint16_t)r[1];
    out->pressure = r[2] / 10.0f;
    return true;
}

static bool _decode_light(const int16_t *r, weather_data_t *out)
{
    if (!r) return false;
    out->light = ((uint32_t)(uint16_t)r[0] << 16) | (uint16_t)r[1];
    return true;
}

/* ── Public API ─────────────────────────────────────────── */

bool decode_weather(const int16_t *wind,
                    const int16_t *thn,
                    const int16_t *pm,
                    const int16_t *light,
                    weather_data_t *out)
{
    memset(out, 0, sizeof(*out));
    bool ok = false;

    if (_decode_wind(wind, out))   ok = true;
    else ESP_LOGW(TAG, "wind block missing/error");

    if (_decode_thn(thn, out))     ok = true;
    else ESP_LOGW(TAG, "temp_hum_noise block missing/error");

    if (_decode_pm(pm, out))       ok = true;
    else ESP_LOGW(TAG, "pm block missing/error");

    if (_decode_light(light, out)) ok = true;
    else ESP_LOGW(TAG, "light block missing/error");

    return ok;
}

cJSON *weather_to_json(const weather_data_t *d)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "wind_speed",      d->wind_speed);
    cJSON_AddStringToObject(obj, "wind_direction",  d->wind_direction);
    cJSON_AddNumberToObject(obj, "wind_angle",      d->wind_angle);
    cJSON_AddNumberToObject(obj, "humidity",        d->humidity);
    cJSON_AddNumberToObject(obj, "temperature",     d->temperature);
    cJSON_AddNumberToObject(obj, "noise",           d->noise);
    cJSON_AddNumberToObject(obj, "pm25",            d->pm25);
    cJSON_AddNumberToObject(obj, "pm10",            d->pm10);
    cJSON_AddNumberToObject(obj, "pressure",        d->pressure);
    cJSON_AddNumberToObject(obj, "light",           d->light);

    return obj;
}
