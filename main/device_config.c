#include <string.h>
#include "device_config.h"
#include "config.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "device_config";

#define NVS_NAMESPACE "devcfg"
#define NVS_KEY_PRESSURE_THRESHOLD "press_thr_mi" /* stored as milli-MPa (int32) */

static float s_pressure_threshold_mpa = PRESSURE_ALARM_THRESHOLD_MPA;

void device_config_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(TAG, "[Config] No saved config yet — using default threshold=%.3f MPa",
                 s_pressure_threshold_mpa);
        return;
    }

    int32_t stored_milli = 0;
    if (nvs_get_i32(h, NVS_KEY_PRESSURE_THRESHOLD, &stored_milli) == ESP_OK)
    {
        s_pressure_threshold_mpa = stored_milli / 1000.0f;
        ESP_LOGI(TAG, "[Config] Loaded pressure_threshold_mpa=%.3f from NVS",
                 s_pressure_threshold_mpa);
    }
    else
    {
        ESP_LOGI(TAG, "[Config] No saved threshold — using default=%.3f MPa",
                 s_pressure_threshold_mpa);
    }

    nvs_close(h);
}

float device_config_get_pressure_threshold_mpa(void)
{
    return s_pressure_threshold_mpa;
}

static void save_pressure_threshold(float mpa)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "[Config] nvs_open failed (%s) — threshold not persisted",
                 esp_err_to_name(err));
        return;
    }

    int32_t milli = (int32_t)(mpa * 1000.0f + (mpa >= 0 ? 0.5f : -0.5f));
    nvs_set_i32(h, NVS_KEY_PRESSURE_THRESHOLD, milli);
    esp_err_t commit_err = nvs_commit(h);
    if (commit_err != ESP_OK)
        ESP_LOGW(TAG, "[Config] nvs_commit failed: %s", esp_err_to_name(commit_err));

    nvs_close(h);
}

bool device_config_apply_json(const char *payload, int payload_len)
{
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root)
    {
        ESP_LOGW(TAG, "[Config] Invalid JSON on %s — ignored", SUB_TOPIC);
        return false;
    }

    bool applied = false;
    bool is_command = cJSON_HasObjectItem(root, "cmd"); /* handled by device_command.c, not us */

    cJSON *thr = cJSON_GetObjectItemCaseSensitive(root, "pressure_threshold_mpa");
    if (cJSON_IsNumber(thr))
    {
        s_pressure_threshold_mpa = (float)thr->valuedouble;
        save_pressure_threshold(s_pressure_threshold_mpa);
        ESP_LOGW(TAG, "[Config] pressure_threshold_mpa -> %.3f MPa (saved)",
                 s_pressure_threshold_mpa);
        applied = true;
    }

    /* Next field goes here, same pattern:
     *
     * cJSON *dbn = cJSON_GetObjectItemCaseSensitive(root, "debounce_s");
     * if (cJSON_IsNumber(dbn)) { ... }
     */

    if (!applied && !is_command)
        ESP_LOGW(TAG, "[Config] JSON on %s had no recognised fields", SUB_TOPIC);

    cJSON_Delete(root);
    return applied;
}