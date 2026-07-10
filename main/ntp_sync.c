#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"

#include "ntp_sync.h"

static const char *TAG = "ntp";

void sync_ntp(time_t *boot_time_out)
{
    ESP_LOGI(TAG, "[Boot] Syncing NTP...");

    if (esp_sntp_enabled())
    {
        ESP_LOGI(TAG, "[Boot] NTP was already running — stopping first");
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "my.pool.ntp.org");
    esp_sntp_setservername(2, "time.cloudflare.com");

    ESP_LOGI(TAG, "[Boot] NTP servers: %s, %s, %s",
             esp_sntp_getservername(0),
             esp_sntp_getservername(1),
             esp_sntp_getservername(2));

    esp_sntp_init();
    ESP_LOGI(TAG, "[Boot] NTP init done, status=%d", esp_sntp_get_sync_status());

    /* Wait up to 30 s. Poll every 500 ms. */
    bool synced = false;
    for (int i = 0; i < 60; i++)
    {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));

        sntp_sync_status_t st = esp_sntp_get_sync_status();
        if (i % 6 == 0)
            ESP_LOGI(TAG, "[Boot] NTP waiting... (%d s) status=%d", i / 2, (int)st);

        if (st == SNTP_SYNC_STATUS_COMPLETED)
        {
            synced = true;
            break;
        }
    }

    if (!synced)
    {
        ESP_LOGW(TAG, "[Boot] NTP sync failed — continuing with local clock");
    }
    else
    {
        time_t now = 0;
        time(&now);
        if (boot_time_out)
            *boot_time_out = now;

        char tbuf[32];
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_info);
        ESP_LOGI(TAG, "[Boot] NTP synced — %s UTC", tbuf);
    }
}