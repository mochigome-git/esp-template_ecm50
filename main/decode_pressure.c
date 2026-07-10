#include <string.h>
#include "decode_pressure.h"
#include "esp_log.h"

static const char *TAG = "pressure";

/* First register of the read (0003H = 小数点 decimal-point config) */
#define PRESSURE_BASE_REG   0x0003
/* 0003H (decimals) + 0004H (raw value) */
#define PRESSURE_REG_COUNT  2

enum {
    OFF_DECIMALS = 0x0003 - PRESSURE_BASE_REG, /* 小数点: 0=int, 1=x.x, 2=x.xx, 3=x.xxx */
    OFF_RAW      = 0x0004 - PRESSURE_BASE_REG, /* 压力输出值, 16-bit unsigned */
};

/* raw / 10^decimals, decimals is 0-3 per the register spec */
static const double PRESSURE_DIVISORS[4] = {1.0, 10.0, 100.0, 1000.0};

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

    /* Both registers are unsigned per the spec — reinterpret the raw
     * int16_t bit pattern as uint16_t rather than treating it as signed. */
    uint16_t decimals = (uint16_t)regs[OFF_DECIMALS];
    uint16_t raw       = (uint16_t)regs[OFF_RAW];

    if (decimals > 3)
    {
        ESP_LOGW(TAG, "[Pressure] slave=%d unexpected decimal code %u (expected 0-3) — treating as 0",
                 slave_addr, decimals);
        decimals = 0;
    }

    out->pressure = raw / PRESSURE_DIVISORS[decimals];
    out->ok = true;
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