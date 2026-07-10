#include <string.h>
#include "device_command.h"
#include "config.h"
#include "machine_run.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "device_command";

#define DEFAULT_SILENCE_S 300

static int64_t s_silence_until_us = 0;
static bool s_silenced = false;

bool device_command_alarm_silenced(void)
{
    if (!s_silenced)
        return false;

    if (esp_timer_get_time() >= s_silence_until_us)
    {
        s_silenced = false;
        ESP_LOGW(TAG, "[Command] Silence window expired -> alarm re-armed");
        return false;
    }

    return true;
}

static void handle_silence_alarm(cJSON *root)
{
    int duration_s = DEFAULT_SILENCE_S;

    cJSON *dur = cJSON_GetObjectItemCaseSensitive(root, "duration_sec");
    if (cJSON_IsNumber(dur) && dur->valuedouble > 0)
        duration_s = (int)dur->valuedouble;

    s_silence_until_us = esp_timer_get_time() + (int64_t)duration_s * 1000000LL;
    s_silenced = true;

    ESP_LOGW(TAG, "[Command] silence_alarm -> muted for %d s", duration_s);
}

static void handle_force_machine_run(cJSON *root)
{
    /* { "cmd": "force_machine_run", "enabled": true, "running": true }
     *
     * enabled=true  -> override engaged, machine_is_running() returns
     *                  `running` (defaults to true if omitted) instead
     *                  of the real power-meter reading.
     * enabled=false -> override cleared, back to the real reading.
     *                  `running` is ignored in this case. */
    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    bool enabled = cJSON_IsBool(en) ? cJSON_IsTrue(en) : true;

    cJSON *run = cJSON_GetObjectItemCaseSensitive(root, "running");
    bool running_state = cJSON_IsBool(run) ? cJSON_IsTrue(run) : true;

    machine_run_set_override(enabled, running_state);

    if (enabled)
        ESP_LOGW(TAG, "[Command] force_machine_run -> override ON, forced running=%s",
                 running_state ? "true" : "false");
    else
        ESP_LOGW(TAG, "[Command] force_machine_run -> override OFF, using real power-meter reading");
}

bool device_command_execute(const char *payload, int payload_len)
{
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root)
        return false; /* not JSON, or already consumed as a settings payload — not an error here */

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    bool executed = false;

    if (cJSON_IsString(cmd) && cmd->valuestring)
    {
        if (strcmp(cmd->valuestring, "silence_alarm") == 0)
        {
            handle_silence_alarm(root);
            executed = true;
        }
        else if (strcmp(cmd->valuestring, "force_machine_run") == 0)
        {
            handle_force_machine_run(root);
            executed = true;
        }
        else
        {
            ESP_LOGW(TAG, "[Command] Unknown cmd '%s' on %s", cmd->valuestring, SUB_TOPIC);
        }
    }

    cJSON_Delete(root);
    return executed;
}