/**
 * esp32s3_gateway — main.c
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "ntp_sync.h"
#include "esp_system.h"
#if __has_include("esp_task_wdt.h")
#include "esp_task_wdt.h"
#else
#include "esp_private/esp_task_wdt.h"
#endif
#include "cJSON.h"
#include "nvs_flash.h"

#include "config.h"
#include "net_utils.h"
#include "mqtt_manager.h"
#include "modbus_utils.h"
#include "relay_control.h"
#include "device_config.h" 
#include "device_command.h"

static const char *TAG = "main";

static time_t s_boot_time = 0;
static modbus_t s_modbus;

static void on_mqtt_message(const char *topic, int topic_len, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "[MQTT] Message on %.*s: %.*s", topic_len, topic, payload_len, payload);
    device_config_apply_json(payload, payload_len);
    device_command_execute(payload, payload_len);
}

/* ══════════════════════════════════════════════════════════
 *  Heartbeat payload builder — gateway-level, not per-device
 *
 *  {
 *    "tenant_id":  "...",
 *    "device_id":  "...",   ← the gateway's own DEVICE_ID from config.h
 *    "status": {
 *      "kind":     "heartbeat",
 *      "fw":       "1.0.0",
 *      "uptime_s": N,
 *      "ts":       "2026-06-16T02:09:12.384Z"   ← ISO-8601 UTC
 *    }
 *  }
 * ══════════════════════════════════════════════════════════ */
static cJSON *build_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "tenant_id", TENANT_ID);
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);
    cJSON_AddNullToObject(root, "machine_id");
    cJSON_AddNullToObject(root, "lot_id");
    cJSON_AddNullToObject(root, "metric_a");
    cJSON_AddNullToObject(root, "metric_b");
    cJSON_AddNullToObject(root, "metric_c");
    cJSON_AddNullToObject(root, "readings");
    cJSON_AddNullToObject(root, "output");
    cJSON_AddNullToObject(root, "limits");
    cJSON_AddNullToObject(root, "energy");

    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "kind", "heartbeat");
    cJSON_AddStringToObject(status, "fw", FW_VERSION);

    time_t now = 0;
    time(&now);
    cJSON_AddNumberToObject(status, "uptime_s", (double)(now - s_boot_time));

    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
    cJSON_AddStringToObject(status, "ts", ts);

    cJSON_AddItemToObject(root, "status", status);
    return root;
}

/* ══════════════════════════════════════════════════════════
 *  Payload builder — fully generic, knows nothing about any specific
 *  device's register map. Telemetry identity (tenant_id, device_id,
 *  machine_id, lot_id) is the same gateway-wide identity from config.h
 *  on every publish; only which field gets filled, and with what,
 *  differs per Modbus slave (see modbus_device_t.payload_field).
 *
 *  {
 *    "tenant_id":  TENANT_ID,
 *    "device_id":  DEVICE_ID,
 *    "machine_id": MACHINE_ID | null,   ← config.h defines this as NULL
 *    "lot_id":     LOT_ID | null,       ←   or a string literal
 *    "metric_a":   null,                ← any of these six become the
 *    "metric_b":   null,                    decoded data if `field_key`
 *    "metric_c":   null,                    names them instead
 *    "readings":   dev->to_json(...) result | null   (default target)
 *    "output":     null,
 *    "limits":     null,
 *    "energy":     null,
 *    "status":     { "rssi": N, "uptime_s": N, "net_mode": "lan"|"wifi" }
 *  }
 *
 *  Takes ownership of `data` (attaches it directly into whichever
 *  field `field_key` names; pass NULL on a failed read to leave that
 *  field as JSON null).
 * ══════════════════════════════════════════════════════════ */
static cJSON *build_payload(const char *field_key, cJSON *data, net_iface_t net_mode, int rssi)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "tenant_id", TENANT_ID);
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);

    /* MACHINE_ID / LOT_ID are #define'd in config.h as either NULL or a
     * string literal, so a plain runtime check is all that's needed —
     * no #ifdef, since the macro is always defined either way. */
    if (MACHINE_ID)
        cJSON_AddStringToObject(root, "machine_id", MACHINE_ID);
    else
        cJSON_AddNullToObject(root, "machine_id");

    if (LOT_ID)
        cJSON_AddStringToObject(root, "lot_id", LOT_ID);
    else
        cJSON_AddNullToObject(root, "lot_id");

    cJSON_AddNullToObject(root, "metric_a");
    cJSON_AddNullToObject(root, "metric_b");
    cJSON_AddNullToObject(root, "metric_c");
    cJSON_AddNullToObject(root, "readings");
    cJSON_AddNullToObject(root, "output");
    cJSON_AddNullToObject(root, "limits");
    cJSON_AddNullToObject(root, "energy");

    if (data)
    {
        /* Swap the decoded data into whichever placeholder field_key
         * names. All six candidate fields exist as null above, so this
         * only fails if field_key is misspelled in config.c. */
        if (!cJSON_ReplaceItemInObject(root, field_key, data))
        {
            ESP_LOGW(TAG, "[Payload] unknown payload_field '%s' — check config.c", field_key);
            cJSON_Delete(data);
        }
    }

    cJSON *status = cJSON_CreateObject();
    cJSON_AddNumberToObject(status, "rssi",
                            (net_mode == NET_IFACE_WIFI) ? rssi : 0);
    time_t now = 0;
    time(&now);
    cJSON_AddNumberToObject(status, "uptime_s", (double)(now - s_boot_time));
    cJSON_AddStringToObject(status, "net_mode",
                            (net_mode == NET_IFACE_LAN) ? "lan" : (net_mode == NET_IFACE_WIFI) ? "wifi"
                                                                                               : "none");
    cJSON_AddItemToObject(root, "status", status);

    return root;
}

/* ══════════════════════════════════════════════════════════
 *  Poll + publish every device in MODBUS_DEVICES, one payload each.
 *
 *  Returns true if ANY device's Modbus read failed (signal for main.c
 *  to reconnect the shared Modbus link). MQTT failure bookkeeping is
 *  handled by the caller since it also needs mqtt_fail_count state.
 * ══════════════════════════════════════════════════════════ */
static bool poll_and_publish_devices(net_iface_t net_mode, int rssi,
                                     bool *any_pub_ok, bool *any_pub_attempted,
                                     uint32_t *device_ticks)   /* ← add parameter */
{
    bool any_read_error = false;
    *any_pub_ok = false;
    *any_pub_attempted = false;

    for (size_t i = 0; i < MODBUS_DEVICE_COUNT; i++)
    {
        const modbus_device_t *dev = &MODBUS_DEVICES[i];

        /* ── Per-device interval gate ── */
        device_ticks[i]++;
        uint32_t interval = dev->publish_interval_s
                            ? dev->publish_interval_s
                            : PUBLISH_INTERVAL_S;
        if (device_ticks[i] < interval)
            continue;
        device_ticks[i] = 0;

        uint8_t scratch[MODBUS_DEVICE_MAX_DATA_SIZE];
        memset(scratch, 0, sizeof(scratch));

        esp_err_t read_err = dev->read(&s_modbus, dev->slave_addr, scratch);
        if (read_err != ESP_OK)
        {
            ESP_LOGW(TAG, "[Modbus] slave=%d (%s) read error: %s",
                     dev->slave_addr, dev->name, esp_err_to_name(read_err));
            any_read_error = true;
        }
        else if (dev->on_data)
        {
            dev->on_data(scratch);
        }
        esp_task_wdt_reset();

        cJSON *data = (read_err == ESP_OK) ? dev->to_json(scratch) : NULL;
        cJSON *payload = build_payload(dev->payload_field, data, net_mode, rssi);

        char *json_str = cJSON_PrintUnformatted(payload);
        ESP_LOGI(TAG, "[Payload] (%s) → %s  %s", dev->name, PUB_TOPIC, json_str ? json_str : "");
        free(json_str);

        esp_task_wdt_reset();
        esp_err_t pub_err = mqtt_publish_json(payload, NULL);
        cJSON_Delete(payload);

        *any_pub_attempted = true;
        if (pub_err == ESP_OK)
        {
            *any_pub_ok = true;
            ESP_LOGI(TAG, "[MQTT] Published OK (%s)", dev->name);
        }
        else
        {
            ESP_LOGW(TAG, "[MQTT] Publish failed (%s)", dev->name);
        }

        esp_task_wdt_reset();
        /* Small gap between devices sharing the same RS-485/TCP link —
         * same role MODBUS_INTER_BLOCK_MS played between register
         * blocks for a single device before. */
        vTaskDelay(pdMS_TO_TICKS(MODBUS_INTER_BLOCK_MS));
    }

    return any_read_error;
}
/* ══════════════════════════════════════════════════════════
 *  Modbus init with retry
 * ══════════════════════════════════════════════════════════ */
static void init_modbus(void)
{
    esp_err_t err;
    if (strcmp(MODBUS_MODE, "rtu") == 0)
    {
        err = modbus_rtu_init(&s_modbus,
                              MODBUS_UART_NUM,
                              MODBUS_PIN_TX, MODBUS_PIN_RX, MODBUS_PIN_DE,
                              MODBUS_BAUDRATE,
                              1000,
                              MODBUS_RETRIES, MODBUS_RETRY_DELAY_MS);
    }
    else
    {
        err = modbus_tcp_connect(&s_modbus,
                                 MODBUS_TCP_SERVER, MODBUS_TCP_PORT,
                                 MODBUS_TCP_TIMEOUT_MS,
                                 MODBUS_RETRIES, MODBUS_RETRY_DELAY_MS);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Modbus init failed: %s", esp_err_to_name(err));
    }
}

/* ══════════════════════════════════════════════════════════
 *  app_main
 * ══════════════════════════════════════════════════════════ */
void app_main(void)
{
    ESP_LOGI(TAG, "[Boot] Starting...");

    /* ── NVS ────────────────────────────────────────────── */
    {
        esp_err_t _nvs = nvs_flash_init();
        if (_nvs == ESP_ERR_NVS_NO_FREE_PAGES ||
            _nvs == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            _nvs = nvs_flash_init();
        }
        ESP_ERROR_CHECK(_nvs);
    }

    /* ── Device config (NVS-backed, MQTT-updatable) ──────── */
    device_config_init();

    /* ── WDT — init FIRST, before any blocking calls ─────
     *
     * CORRECT ORDER:
     *   1. Configure & start WDT
     *   2. Subscribe this task  (esp_task_wdt_add)
     *   3. NTP sync             (feeds WDT internally)
     *   4. MQTT init
     *   5. Main loop            (feeds WDT every tick)
     * ─────────────────────────────────────────────────────── */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0, /* don't watch idle tasks */
        .trigger_panic = true,
    };
    {
        esp_err_t _wdt = esp_task_wdt_reconfigure(&wdt_cfg);
        if (_wdt == ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));
        else
            ESP_ERROR_CHECK(_wdt);
    }
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL)); /* subscribe THIS task */
    ESP_LOGI(TAG, "[Boot] WDT started (%d s)", WDT_TIMEOUT_S);

    /* ── Network ─────────────────────────────────────────── */
    esp_task_wdt_reset();
    net_connect(NET_MODE, WIFI_SSID, WIFI_PASS);

    /* ── Internet test ───────────────────────────────────── */
    esp_task_wdt_reset();
    test_internet(); /* informational — does NOT block boot */

    /* ── NTP ─────────────────────────────────────────────── */
    esp_task_wdt_reset();
    sync_ntp(&s_boot_time);
    if (s_boot_time == 0)
        time(&s_boot_time);

    /* ── MQTT ────────────────────────────────────────────── */
    esp_task_wdt_reset();
    ssl_mode_t ssl_mode = SSL_NONE;
    if (strcmp(SSL_MODE, "mutual") == 0)
        ssl_mode = SSL_MUTUAL;
    else if (strcmp(SSL_MODE, "server") == 0)
        ssl_mode = SSL_SERVER;

    mqtt_broker_cfg_t broker = {
        .client_id = CLIENT_ID,
        .broker = BROKER,
        .port = USE_SSL ? PORT_MQTTS : PORT_MQTT,
        .user = MQTT_USER,
        .password = MQTT_PASSWORD,
        .ssl_mode = ssl_mode,
        .keepalive_s = 60,
    };
    mqtt_topic_cfg_t topics = {
        .sub_topic = SUB_TOPIC,
        .pub_topic = PUB_TOPIC,
        .on_message = on_mqtt_message,
    };

    /* mqtt_manager_init() does a blocking TLS connect which can take
     * 8–15 s on a slow link — longer than WDT_TIMEOUT_S.
     * Temporarily unsubscribe this task so the WDT doesn't fire,
     * then re-subscribe immediately after.                          */
    esp_task_wdt_delete(NULL);
    ESP_ERROR_CHECK(mqtt_manager_init(&broker, &topics));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    /* ── Modbus ──────────────────────────────────────────── */
    esp_task_wdt_reset();
    init_modbus();
    ESP_LOGI(TAG, "[Boot] %d Modbus device(s) configured", (int)MODBUS_DEVICE_COUNT);

    /* ── Relay / alarm outputs ───────────────────────────── */
    esp_task_wdt_reset();
    if (relay_control_init() != ESP_OK)
        ESP_LOGE(TAG, "[Boot] Relay init failed — DO1/DO2 may be unusable");

    /* ══════════════════════════════════════════════════════
     *  Main loop
     * ══════════════════════════════════════════════════════ */
    uint32_t device_ticks[MODBUS_DEVICE_COUNT];
    memset(device_ticks, 0, sizeof(device_ticks));
    int net_check_tick = 0;
    int heartbeat_tick = 0;
    int net_fail_count = 0;
    int mqtt_fail_count = 0;
    net_iface_t last_net_mode = NET_IFACE_NONE;
    int last_rssi = 0;

    ESP_LOGI(TAG, "[Boot] Main loop started");

    while (1)
    {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
        net_check_tick++;
        heartbeat_tick++;

        /* ── Network health check ────────────────────────── */
        if (net_check_tick >= NET_CHECK_INTERVAL_S)
        {
            net_check_tick = 0;
            int rssi = 0;
            net_iface_t iface = net_check(&rssi);
            last_net_mode = iface;
            last_rssi = rssi;

            if (iface != NET_IFACE_NONE)
            {
                net_fail_count = 0;
            }
            else
            {
                net_fail_count++;
                ESP_LOGW(TAG, "[Net] Down (%d/%d)", net_fail_count, NET_FAIL_RESET_COUNT);
                if (net_fail_count >= NET_FAIL_RESET_COUNT)
                    hard_reset("network down");
            }
        }

        esp_task_wdt_reset();

        /* ── Heartbeat  (every HEARTBEAT_INTERVAL_S seconds) ── */
        if (heartbeat_tick >= HEARTBEAT_INTERVAL_S)
        {
            heartbeat_tick = 0;
            esp_task_wdt_reset();

            cJSON *hb = build_heartbeat();
            esp_err_t hb_err = mqtt_publish_json(hb, NULL);
            cJSON_Delete(hb);

            if (hb_err == ESP_OK)
                ESP_LOGI(TAG, "[HB] Heartbeat sent");
            else
                ESP_LOGW(TAG, "[HB] Heartbeat failed");
        }

       /* ── Publish cycle ── per-device interval gates inside the function ── */
        esp_task_wdt_reset();

        bool any_pub_ok, any_pub_attempted;
        bool any_read_error = poll_and_publish_devices(last_net_mode, last_rssi,
                                                    &any_pub_ok, &any_pub_attempted,
                                                    device_ticks);

        if (any_pub_attempted && !any_pub_ok)
        {
            mqtt_fail_count++;
            ESP_LOGW(TAG, "[MQTT] All publishes failed this cycle (%d/%d)",
                    mqtt_fail_count, MQTT_FAIL_RESET_COUNT);
            if (mqtt_fail_count >= MQTT_FAIL_RESET_COUNT)
                hard_reset("MQTT publish repeatedly failed");
        }
        else if (any_pub_attempted)   /* only reset counter when we actually tried */
        {
            mqtt_fail_count = 0;
        }

        if (any_read_error)
        {
            esp_task_wdt_reset();
            ESP_LOGW(TAG, "[Modbus] Errors — reconnecting...");
            modbus_close(&s_modbus);
            init_modbus();
        }
    }
}