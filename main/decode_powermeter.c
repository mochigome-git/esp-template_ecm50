#include <string.h>
#include "decode_powermeter.h"
#include "esp_log.h"

static const char *TAG = "powermeter";

/* First register of the read (0x1000 = A相电压有效值) */
#define PM_BASE_REG   0x1000
/* Registers spanning 0x1000 .. 0x106B inclusive (low word of the last field) */
#define PM_REG_COUNT  108

/* Offsets in *registers* (16-bit words) from PM_BASE_REG.
 * NOTE: assumes the meter's data-format register (000CH low byte) is left
 * at its default 0 = "INT ABCD" (reg[0]=high word, reg[1]=low word).
 * If a given slave is configured for CDAB/BADC/DCBA, reg_pair_to_i32()
 * below needs to swap words/bytes accordingly for that slave. */
enum {
    OFF_V_RMS                  = 0x1000 - PM_BASE_REG, /* A相电压有效值 */
    OFF_I_RMS                  = 0x1006 - PM_BASE_REG, /* A相电流有效值 */
    OFF_P_ACTIVE                = 0x1014 - PM_BASE_REG, /* 合相有功功率 */
    OFF_P_REACTIVE               = 0x101C - PM_BASE_REG, /* 合相无功功率 */
    OFF_P_APPARENT               = 0x1024 - PM_BASE_REG, /* 合相视在功率 */
    OFF_POWER_FACTOR             = 0x102C - PM_BASE_REG, /* 合相功率因数 */
    OFF_FREQUENCY                = 0x1034 - PM_BASE_REG, /* 电网频率 */
    OFF_ENERGY_ACTIVE_TOTAL      = 0x103C - PM_BASE_REG, /* 合相有功总电能 */
    OFF_ENERGY_REACTIVE_TOTAL    = 0x104A - PM_BASE_REG, /* 合相无功总电能 */
    OFF_ENERGY_IMPORT            = 0x1052 - PM_BASE_REG, /* 合相正向有功电能 */
    OFF_ENERGY_EXPORT            = 0x105A - PM_BASE_REG, /* 合相反向有功电能 */
    OFF_ENERGY_REACTIVE_IMPORT   = 0x1062 - PM_BASE_REG, /* 合相正向无功电能 */
    OFF_ENERGY_REACTIVE_EXPORT   = 0x106A - PM_BASE_REG, /* 合相反向无功电能 */
};

/* Combine two big-endian 16-bit holding registers into a signed int32. */
static inline int32_t reg_pair_to_i32(const int16_t *regs, int offset)
{
    uint32_t hi = (uint16_t)regs[offset];
    uint32_t lo = (uint16_t)regs[offset + 1];
    return (int32_t)((hi << 16) | lo);
}

esp_err_t powermeter_read(modbus_t *mb, uint8_t slave_addr, powermeter_data_t *out)
{
    memset(out, 0, sizeof(*out));

    int16_t regs[PM_REG_COUNT];
    esp_err_t err = modbus_read_holding(mb, slave_addr, PM_BASE_REG, PM_REG_COUNT, regs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "[PM] slave=%d read failed: %s", slave_addr, esp_err_to_name(err));
        out->ok = false;
        return err;
    }

    out->v_rms                        = reg_pair_to_i32(regs, OFF_V_RMS)                / 100.0;
    out->i_rms                        = reg_pair_to_i32(regs, OFF_I_RMS)                / 1000.0;
    out->p_active_kw                  = reg_pair_to_i32(regs, OFF_P_ACTIVE)             / 10000.0;
    out->p_reactive_kvar              = reg_pair_to_i32(regs, OFF_P_REACTIVE)           / 10000.0;
    out->p_apparent_kva               = reg_pair_to_i32(regs, OFF_P_APPARENT)           / 10000.0;
    out->power_factor                 = reg_pair_to_i32(regs, OFF_POWER_FACTOR)         / 1000.0;
    out->frequency_hz                 = reg_pair_to_i32(regs, OFF_FREQUENCY)            / 100.0;
    out->energy_active_total_kwh      = reg_pair_to_i32(regs, OFF_ENERGY_ACTIVE_TOTAL)  / 1000.0;
    out->energy_import_kwh            = reg_pair_to_i32(regs, OFF_ENERGY_IMPORT)        / 1000.0;
    out->energy_export_kwh            = reg_pair_to_i32(regs, OFF_ENERGY_EXPORT)        / 1000.0;
    out->energy_reactive_total_kvarh  = reg_pair_to_i32(regs, OFF_ENERGY_REACTIVE_TOTAL)  / 1000.0;
    out->energy_reactive_import_kvarh = reg_pair_to_i32(regs, OFF_ENERGY_REACTIVE_IMPORT) / 1000.0;
    out->energy_reactive_export_kvarh = reg_pair_to_i32(regs, OFF_ENERGY_REACTIVE_EXPORT) / 1000.0;

    out->ok = true;
    return ESP_OK;
}

cJSON *powermeter_to_json(const powermeter_data_t *d)
{
    if (!d->ok)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v_rms", d->v_rms);
    cJSON_AddNumberToObject(root, "i_rms", d->i_rms);
    cJSON_AddNumberToObject(root, "p_active_kw", d->p_active_kw);
    cJSON_AddNumberToObject(root, "p_reactive_kvar", d->p_reactive_kvar);
    cJSON_AddNumberToObject(root, "p_apparent_kva", d->p_apparent_kva);
    cJSON_AddNumberToObject(root, "power_factor", d->power_factor);
    cJSON_AddNumberToObject(root, "frequency_hz", d->frequency_hz);
    cJSON_AddNumberToObject(root, "energy_import_kwh", d->energy_import_kwh);
    cJSON_AddNumberToObject(root, "energy_export_kwh", d->energy_export_kwh);
    cJSON_AddNumberToObject(root, "energy_active_total_kwh", d->energy_active_total_kwh);
    cJSON_AddNumberToObject(root, "energy_reactive_import_kvarh", d->energy_reactive_import_kvarh);
    cJSON_AddNumberToObject(root, "energy_reactive_export_kvarh", d->energy_reactive_export_kvarh);
    cJSON_AddNumberToObject(root, "energy_reactive_total_kvarh", d->energy_reactive_total_kvarh);
    return root;
}

esp_err_t powermeter_read_generic(modbus_t *mb, uint8_t slave_addr, void *out)
{
    return powermeter_read(mb, slave_addr, (powermeter_data_t *)out);
}

cJSON *powermeter_to_json_generic(const void *data)
{
    return powermeter_to_json((const powermeter_data_t *)data);
}