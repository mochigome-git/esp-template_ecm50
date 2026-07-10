#include <stdbool.h>
#include "esp_timer.h"
#include "pressure_alarm.h"
#include "decode_pressure.h"
#include "relay_control.h"
#include "machine_run.h"
#include "device_config.h"
#include "device_command.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "pressure_alarm";

/* Only log on state changes, not every publish cycle. */
static bool s_last_tripped = false;
static bool s_have_state = false;
static bool s_last_silenced = false;

/* Debounce: pressure must stay <= threshold continuously for
 * PRESSURE_ALARM_DEBOUNCE_S before the buzzer actually trips. Reset
 * any time pressure recovers above threshold or the machine stops. */
static int64_t s_low_since_us = 0;
static bool s_low_since_valid = false;

void pressure_alarm_check(const void *data)
{
    const pressure_data_t *p = (const pressure_data_t *)data;

    if (!p->ok)
    {
        /* Read failed upstream — leave the relay in whatever state it
         * was last in rather than guessing. */
        ESP_LOGW(TAG, "[Alarm] pressure data invalid — relay left unchanged");
        return;
    }

    /* ── Machine-run gate ─────────────────────────────────
     * If the power meter says the machine isn't drawing current,
     * near-zero pressure is expected, not a fault. Force the buzzer
     * off and clear the debounce timer so a fresh sustained-low
     * period is required once the machine actually restarts. */
    if (!machine_is_running())
    {
        if (!s_have_state || s_last_tripped)
            ESP_LOGI(TAG, "[Alarm] machine stopped -> buzzer forced OFF");

        relay_alarm_set(false);
        s_last_tripped = false;
        s_have_state = true;
        s_low_since_valid = false;
        return;
    }

    float threshold = device_config_get_pressure_threshold_mpa();
    bool below = (p->pressure <= threshold);

    /* ── Debounce ─────────────────────────────────────────
     * Start (or keep) a timer the instant pressure drops below
     * threshold; only trip once it's stayed low continuously for
     * PRESSURE_ALARM_DEBOUNCE_S. Any reading back above threshold
     * cancels the timer immediately, requiring a fresh full debounce
     * period next time it dips. */
    if (!below)
    {
        s_low_since_valid = false;
    }
    else if (!s_low_since_valid)
    {
        s_low_since_valid = true;
        s_low_since_us = esp_timer_get_time();
    }

    bool tripped = below && s_low_since_valid &&
                   (esp_timer_get_time() - s_low_since_us) >=
                       (int64_t)PRESSURE_ALARM_DEBOUNCE_S * 1000000LL;

    /* ── Remote silence ───────────────────────────────────
     * A "silence_alarm" MQTT command mutes the physical buzzer for a
     * bounded window without touching `tripped` itself — so the
     * moment the window expires, a still-tripped condition sounds
     * again automatically. No physical button needed, and it can't be
     * silenced forever by one stale command. */
    bool silenced = tripped && device_command_alarm_silenced();
    bool buzzer_on = tripped && !silenced;

    if (!s_have_state || tripped != s_last_tripped)
    {
        ESP_LOGW(TAG, "[Alarm] pressure=%.3f MPa threshold=%.3f MPa -> buzzer %s",
                 p->pressure, threshold, tripped ? "ON" : "OFF");
    }
    if (silenced != s_last_silenced)
    {
        ESP_LOGW(TAG, "[Alarm] remote silence %s", silenced ? "engaged" : "cleared");
    }

    relay_alarm_set(buzzer_on);
    s_last_tripped = tripped;
    s_last_silenced = silenced;
    s_have_state = true;
}