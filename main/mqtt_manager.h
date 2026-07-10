#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/**
 * SSL mode — mirrors ssl_utils.py load_ssl_params(mode=...)
 */
typedef enum {
    SSL_NONE,
    SSL_SERVER,
    SSL_MUTUAL,
} ssl_mode_t;

typedef struct {
    const char *client_id;
    const char *broker;
    int         port;
    const char *user;
    const char *password;
    ssl_mode_t  ssl_mode;
    int         keepalive_s;
} mqtt_broker_cfg_t;

typedef struct {
    const char *sub_topic;
    const char *pub_topic;
    void      (*on_message)(const char *topic, int topic_len,
                            const char *payload, int payload_len);
} mqtt_topic_cfg_t;

/**
 * Initialise and connect. Loads certs from /spiffs/certs/ if needed.
 * Starts a background FreeRTOS task for ping + incoming message polling
 * (mirrors MQTTManager.start_loop() in Python).
 */
esp_err_t mqtt_manager_init(const mqtt_broker_cfg_t *broker,
                             const mqtt_topic_cfg_t  *topics);

/**
 * Publish cJSON payload. NULL topic → use default pub_topic.
 * Thread-safe (mutex protected).
 */
esp_err_t mqtt_publish_json(cJSON *payload, const char *topic);

bool mqtt_is_connected(void);
