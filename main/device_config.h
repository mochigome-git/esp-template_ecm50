#pragma once
#include <stdbool.h>

/**
 * Runtime-configurable device settings, persisted to NVS so they
 * survive reboot. Defaults come from config.h; incoming JSON on
 * SUB_TOPIC updates them live via device_config_apply_json().
 *
 * This is deliberately dumb right now — no ack publish, no schema
 * validation beyond "is it a number" — since the backend/DB side that
 * will drive it doesn't exist yet. Add fields the same way
 * pressure_threshold_mpa was added: one cJSON_GetObjectItemCaseSensitive()
 * block per key.
 */

/** Call once at boot, after nvs_flash_init(). Loads saved values from
 *  NVS, or falls back to the config.h defaults if nothing is saved yet. */
void device_config_init(void);

float device_config_get_pressure_threshold_mpa(void);

/**
 * Parses an incoming JSON settings payload (the raw MQTT message body
 * from SUB_TOPIC) and applies any recognised fields, persisting
 * changes to NVS as they're applied.
 *
 * Recognised fields so far:
 *   { "pressure_threshold_mpa": 0.35 }
 *
 * @return true if at least one recognised field was applied.
 */
bool device_config_apply_json(const char *payload, int payload_len);