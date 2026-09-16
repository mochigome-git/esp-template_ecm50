#include <string.h>
#include "decode_powermeter.h"
#include "esp_log.h"

static const char *TAG = "powermeter";

/* First register of the read (0x0FFE = 模块温度, extended down from 0x1000
 * to capture module_temp_c in the same burst read as everything else). */
#define PM_BASE_REG 0x0FFE
/* Registers spanning 0x0FFE .. 0x106B inclusive (low word of the last field) */
#define PM_REG_COUNT 110

/* Offsets in *registers* (16-bit words) from PM_BASE_REG.
 * NOTE: assumes the meter's data-format register (000CH low byte) is left
 * at its default 0 = "INT ABCD" (reg[0]=high word, reg[1]=low word). */
enum
{
    OFF_MODULE_TEMP = 0x0FFE - PM_BASE_REG, /* 模块温度 (not true ambient — correlates with load) */

    OFF_V_RMS_A = 0x1000 - PM_BASE_REG, /* A相电压 */
    OFF_V_RMS_B = 0x1002 - PM_BASE_REG, /* B相电压 */
    OFF_V_RMS_C = 0x1004 - PM_BASE_REG, /* C相电压 */

    OFF_I_RMS_A = 0x1006 - PM_BASE_REG, /* A相电流 */
    OFF_I_RMS_B = 0x1008 - PM_BASE_REG, /* B相电流 */
    OFF_I_RMS_C = 0x100A - PM_BASE_REG, /* C相电流 */

    OFF_LEAKAGE_CURRENT = 0x100C - PM_BASE_REG, /* 0序(漏电流)电流 — ground fault / insulation alert */

    OFF_P_ACTIVE = 0x1014 - PM_BASE_REG,   /* 合相有功功率 */
    OFF_P_REACTIVE = 0x101C - PM_BASE_REG, /* 合相无功功率 */
    OFF_P_APPARENT = 0x1024 - PM_BASE_REG, /* 合相视在功率 */

    OFF_PF_A = 0x1026 - PM_BASE_REG,         /* A相功率因数 */
    OFF_PF_B = 0x1028 - PM_BASE_REG,         /* B相功率因数 */
    OFF_PF_C = 0x102A - PM_BASE_REG,         /* C相功率因数 */
    OFF_POWER_FACTOR = 0x102C - PM_BASE_REG, /* 合相功率因数 */

    OFF_ANGLE_A = 0x102E - PM_BASE_REG, /* A相电压相角 — reference, always 0° */
    OFF_ANGLE_B = 0x1030 - PM_BASE_REG, /* B相电压相角 — should be ~120° from A */
    OFF_ANGLE_C = 0x1032 - PM_BASE_REG, /* C相电压相角 — should be ~240° from A */

    OFF_FREQUENCY = 0x1034 - PM_BASE_REG, /* 电网频率 */

    /* NOTE: kept at 0x1042 per the original tested comment ("NOT 0x103C
     * which is A相 only"). If your meter/datasheet revision differs from
     * that finding, verify against real hardware before switching. */
    OFF_ENERGY_ACTIVE_TOTAL = 0x1042 - PM_BASE_REG,    /* 合相有功总电能 */
    OFF_ENERGY_REACTIVE_TOTAL = 0x104A - PM_BASE_REG,  /* 合相无功总电能 */
    OFF_ENERGY_IMPORT = 0x1052 - PM_BASE_REG,          /* 合相正向有功电能 */
    OFF_ENERGY_EXPORT = 0x105A - PM_BASE_REG,          /* 合相反向有功电能 */
    OFF_ENERGY_REACTIVE_IMPORT = 0x1062 - PM_BASE_REG, /* 合相正向无功电能 */
    OFF_ENERGY_REACTIVE_EXPORT = 0x106A - PM_BASE_REG, /* 合相反向无功电能 */
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

    /* Per-phase voltage — now exposed individually for voltage_imbalance_pct */
    out->v_a = reg_pair_to_i32(regs, OFF_V_RMS_A) / 100.0;
    out->v_b = reg_pair_to_i32(regs, OFF_V_RMS_B) / 100.0;
    out->v_c = reg_pair_to_i32(regs, OFF_V_RMS_C) / 100.0;
    /* Kept for backward compatibility with existing consumers of v_rms */
    out->v_rms = out->v_a;

    /* Per-phase current — now exposed individually for current_imbalance_pct
     * and overload detection, in addition to the summed total. */
    out->i_a = reg_pair_to_i32(regs, OFF_I_RMS_A) / 1000.0;
    out->i_b = reg_pair_to_i32(regs, OFF_I_RMS_B) / 1000.0;
    out->i_c = reg_pair_to_i32(regs, OFF_I_RMS_C) / 1000.0;
    /* Kept for backward compatibility with existing consumers of i_rms */
    out->i_rms = out->i_a + out->i_b + out->i_c;

    out->leakage_current_a = reg_pair_to_i32(regs, OFF_LEAKAGE_CURRENT) / 1000.0;

    out->p_active_kw = reg_pair_to_i32(regs, OFF_P_ACTIVE) / 10000.0;
    out->p_reactive_kvar = reg_pair_to_i32(regs, OFF_P_REACTIVE) / 10000.0;
    out->p_apparent_kva = reg_pair_to_i32(regs, OFF_P_APPARENT) / 10000.0;

    out->pf_a = reg_pair_to_i32(regs, OFF_PF_A) / 1000.0;
    out->pf_b = reg_pair_to_i32(regs, OFF_PF_B) / 1000.0;
    out->pf_c = reg_pair_to_i32(regs, OFF_PF_C) / 1000.0;
    out->power_factor = reg_pair_to_i32(regs, OFF_POWER_FACTOR) / 1000.0;

    out->angle_a = reg_pair_to_i32(regs, OFF_ANGLE_A) / 100.0;
    out->angle_b = reg_pair_to_i32(regs, OFF_ANGLE_B) / 100.0;
    out->angle_c = reg_pair_to_i32(regs, OFF_ANGLE_C) / 100.0;

    out->frequency_hz = reg_pair_to_i32(regs, OFF_FREQUENCY) / 100.0;

    out->energy_active_total_kwh = reg_pair_to_i32(regs, OFF_ENERGY_ACTIVE_TOTAL) / 1000.0;
    out->energy_reactive_total_kvarh = reg_pair_to_i32(regs, OFF_ENERGY_REACTIVE_TOTAL) / 1000.0;
    out->energy_import_kwh = reg_pair_to_i32(regs, OFF_ENERGY_IMPORT) / 1000.0;
    out->energy_export_kwh = reg_pair_to_i32(regs, OFF_ENERGY_EXPORT) / 1000.0;
    out->energy_reactive_import_kvarh = reg_pair_to_i32(regs, OFF_ENERGY_REACTIVE_IMPORT) / 1000.0;
    out->energy_reactive_export_kvarh = reg_pair_to_i32(regs, OFF_ENERGY_REACTIVE_EXPORT) / 1000.0;

    out->module_temp_c = reg_pair_to_i32(regs, OFF_MODULE_TEMP) / 100.0;

    out->ok = true;
    return ESP_OK;
}

cJSON *powermeter_to_json(const powermeter_data_t *d)
{
    if (!d->ok)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v_rms", d->v_rms);
    cJSON_AddNumberToObject(root, "v_a", d->v_a);
    cJSON_AddNumberToObject(root, "v_b", d->v_b);
    cJSON_AddNumberToObject(root, "v_c", d->v_c);

    cJSON_AddNumberToObject(root, "i_rms", d->i_rms);
    cJSON_AddNumberToObject(root, "i_a", d->i_a);
    cJSON_AddNumberToObject(root, "i_b", d->i_b);
    cJSON_AddNumberToObject(root, "i_c", d->i_c);

    cJSON_AddNumberToObject(root, "leakage_current_a", d->leakage_current_a);

    cJSON_AddNumberToObject(root, "p_active_kw", d->p_active_kw);
    cJSON_AddNumberToObject(root, "p_reactive_kvar", d->p_reactive_kvar);
    cJSON_AddNumberToObject(root, "p_apparent_kva", d->p_apparent_kva);

    cJSON_AddNumberToObject(root, "power_factor", d->power_factor);
    cJSON_AddNumberToObject(root, "pf_a", d->pf_a);
    cJSON_AddNumberToObject(root, "pf_b", d->pf_b);
    cJSON_AddNumberToObject(root, "pf_c", d->pf_c);

    cJSON_AddNumberToObject(root, "angle_a", d->angle_a);
    cJSON_AddNumberToObject(root, "angle_b", d->angle_b);
    cJSON_AddNumberToObject(root, "angle_c", d->angle_c);

    cJSON_AddNumberToObject(root, "frequency_hz", d->frequency_hz);

    cJSON_AddNumberToObject(root, "energy_import_kwh", d->energy_import_kwh);
    cJSON_AddNumberToObject(root, "energy_export_kwh", d->energy_export_kwh);
    cJSON_AddNumberToObject(root, "energy_active_total_kwh", d->energy_active_total_kwh);
    cJSON_AddNumberToObject(root, "energy_reactive_import_kvarh", d->energy_reactive_import_kvarh);
    cJSON_AddNumberToObject(root, "energy_reactive_export_kvarh", d->energy_reactive_export_kvarh);
    cJSON_AddNumberToObject(root, "energy_reactive_total_kvarh", d->energy_reactive_total_kvarh);

    cJSON_AddNumberToObject(root, "module_temp_c", d->module_temp_c);
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