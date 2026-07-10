#include "relay_control.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "relay";

/* Set to 0 here (and nowhere else) if your relay/buzzer board is
 * active-low (GPIO low = energized/on) instead of active-high. */
#define RELAY_ACTIVE_LEVEL 1

static inline void set_do(int pin, bool on)
{
    gpio_set_level(pin, on ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
}

esp_err_t relay_control_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RELAY_PIN_DO1) | (1ULL << RELAY_PIN_DO2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "[Relay] gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    set_do(RELAY_PIN_DO1, false);
    set_do(RELAY_PIN_DO2, false);

    ESP_LOGI(TAG, "[Relay] DO1=%d DO2=%d ready", RELAY_PIN_DO1, RELAY_PIN_DO2);
    return ESP_OK;
}

void relay_alarm_set(bool active)
{
    set_do(RELAY_PIN_DO1, active);
    set_do(RELAY_PIN_DO2, active);
}