#include <string.h>
#include "decode_pressure.h"
#include "esp_log.h"

static const char *TAG = "pressure";

#define PRESSURE_BASE_REG   0x0002
#define PRESSURE_REG_COUNT  3       /* 0x0002 unit, 0x0003 decimals, 0x0004 raw */

enum {
    OFF_UNIT     = 0x0002 - PRESSURE_BASE_REG,
    OFF_DECIMALS = 0x0003 - PRESSURE_BASE_REG,
    OFF_RAW      = 0x0004 - PRESSURE_BASE_REG,
};

static const double PRESSURE_DIVISORS[4] = {1.0, 10.0, 100.0, 1000.0};

/* Conversion factors to MPa for each unit code in register 0x0002.
 * Index matches: 0=MPa 1=kPa 2=Pa 3=Bar 4=mBar 5=kg/cm2 6=psi 7=mH2O 8=mmH2O */
static const double UNIT_TO_MPA[9] = {
    1.0,            /* 0: MPa        → MPa  ×1          */
    1.0 / 1000.0,   /* 1: kPa        → MPa  ÷1000       */
    1.0 / 1000000.0,/* 2: Pa         → MPa  ÷1000000    */
    0.1,            /* 3: Bar        → MPa  ×0.1        */
    0.0001,         /* 4: mBar       → MPa  ×0.0001     */
    0.0980665,      /* 5: kg/cm²     → MPa  ×0.0980665  */
    0.00689476,     /* 6: psi        → MPa  ×0.00689476 */
    0.00980665,     /* 7: mH2O       → MPa  ×0.00980665 */
    0.00000980665,  /* 8: mmH2O      → MPa  ×9.80665e-6 */
};

/* Rolling average over N samples */
#define PRESSURE_AVG_SAMPLES 5

static double  s_buf[PRESSURE_AVG_SAMPLES];
static uint8_t s_count = 0;
static uint8_t s_head  = 0;

esp_err_t pressure_read(modbus_t *mb, uint8_t slave_addr, pressure_data_t *out)
{
    memset(out, 0, sizeof(*out));

    int16_t regs[PRESSURE_REG_COUNT];
    esp_err_t err = modbus_read_holding(mb, slave_addr, PRESSURE_BASE_REG, PRESSURE_REG_COUNT, regs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "[Pressure] slave=%d read failed: %s", slave_addr, esp_err_to_name(err));
        out->ok = false;
        return err;
    }

    uint16_t unit     = (uint16_t)regs[OFF_UNIT];
    uint16_t decimals = (uint16_t)regs[OFF_DECIMALS];
    uint16_t raw      = (uint16_t)regs[OFF_RAW];

    if (unit > 8)
    {
        ESP_LOGW(TAG, "[Pressure] slave=%d unexpected unit code %u (expected 0-8) — treating as MPa",
                 slave_addr, unit);
        unit = 0;
    }
    if (decimals > 3)
    {
        ESP_LOGW(TAG, "[Pressure] slave=%d unexpected decimal code %u (expected 0-3) — treating as 0",
                 slave_addr, decimals);
        decimals = 0;
    }

    /* Apply decimal shift first, then convert native unit → MPa */
    double native = raw / PRESSURE_DIVISORS[decimals];
    double sample  = native * UNIT_TO_MPA[unit];

    ESP_LOGI(TAG, "[Pressure] slave=%d unit=%u decimals=%u raw=%u native=%.4f sample=%.6f MPa",
             slave_addr, unit, decimals, raw, native, sample);

    /* Push into ring buffer */
    s_buf[s_head] = sample;
    s_head = (s_head + 1) % PRESSURE_AVG_SAMPLES;
    if (s_count < PRESSURE_AVG_SAMPLES)
        s_count++;

    /* Suppress output until buffer is full */
    if (s_count < PRESSURE_AVG_SAMPLES)
    {
        ESP_LOGI(TAG, "[Pressure] warming up (%u/%u samples)", s_count, PRESSURE_AVG_SAMPLES);
        out->ok = false;
        return ESP_OK;
    }

    double sum = 0.0;
    for (uint8_t i = 0; i < PRESSURE_AVG_SAMPLES; i++)
        sum += s_buf[i];

    out->pressure = sum / PRESSURE_AVG_SAMPLES;
    out->ok = true;

    ESP_LOGI(TAG, "[Pressure] slave=%d smoothed=%.6f MPa", slave_addr, out->pressure);

    return ESP_OK;
}

cJSON *pressure_to_json(const pressure_data_t *d)
{
    if (!d->ok)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "pressure", d->pressure);
    return root;
}

esp_err_t pressure_read_generic(modbus_t *mb, uint8_t slave_addr, void *out)
{
    return pressure_read(mb, slave_addr, (pressure_data_t *)out);
}

cJSON *pressure_to_json_generic(const void *data)
{
    return pressure_to_json((const pressure_data_t *)data);
}