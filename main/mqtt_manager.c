/**
 * mqtt_manager.c
 *
 * Hand-rolled MQTT v3.1.1 client over BSD sockets + mbedTLS.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_task_wdt.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "esp_partition.h"

#include "esp_tls.h"
#include "esp_crt_bundle.h"

#include "cJSON.h"
#include "mqtt_manager.h"

static const char *TAG = "mqtt_mgr";

/* ── Manager state ───────────────────────────────────────── */

static esp_tls_t *s_tls_handle = NULL;
static int s_sock = -1;
static volatile bool s_connected = false;
static SemaphoreHandle_t s_lock = NULL;

static mqtt_broker_cfg_t s_broker;
static mqtt_topic_cfg_t s_topics;
static char s_pub_topic[128];

static char *s_ca_cert = NULL;
static char *s_client_cert = NULL;
static char *s_client_key = NULL;

/* ══════════════════════════════════════════════════════════
 *  SPIFFS cert loader
 * ══════════════════════════════════════════════════════════ */

static char *load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        ESP_LOGW(TAG, "Cert not found: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    ESP_LOGI(TAG, "Loaded %s (%ld B)", path, sz);
    return buf;
}

static void load_certs(ssl_mode_t mode)
{
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (it)
    {
        const esp_partition_t *p = esp_partition_get(it);
        ESP_LOGI(TAG, "Found SPIFFS partition: label='%s' offset=0x%x size=0x%x",
                 p->label, p->address, p->size);
        esp_partition_iterator_release(it);
    }
    else
    {
        ESP_LOGE(TAG, "No SPIFFS partition found!");
        return;
    }

    esp_vfs_spiffs_conf_t cfg = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    if (mode == SSL_SERVER || mode == SSL_MUTUAL)
        s_ca_cert = load_file("/spiffs/ca.pem");
    if (mode == SSL_MUTUAL)
    {
        s_client_cert = load_file("/spiffs/client.crt");
        s_client_key = load_file("/spiffs/client.key");
    }
}

/* ══════════════════════════════════════════════════════════
 *  Low-level read / write
 * ══════════════════════════════════════════════════════════ */

static int net_write(const uint8_t *buf, size_t len)
{
    if (s_tls_handle)
        return esp_tls_conn_write(s_tls_handle, buf, len);
    return send(s_sock, buf, len, 0);
}

static int net_read(uint8_t *buf, size_t len)
{
    if (s_tls_handle)
    {
        int r = esp_tls_conn_read(s_tls_handle, buf, len);
        /* SO_RCVTIMEO fires as EAGAIN/EWOULDBLOCK — treat as "no data",
         * not as a connection error.  Callers that need exact bytes
         * (net_read_exact) will retry; check_msg_nonblocking will bail. */
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == 0)
            return 0;
        if (r < 0)
        {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK)
                return 0; /* timeout — no data, not an error */
        }
        return r;
    }
    return recv(s_sock, buf, len, 0);
}

static int net_read_exact(uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len)
    {
        int r = net_read(buf + got, len - got);
        if (r <= 0)
            return -1;
        got += r;
    }
    return (int)got;
}

/* ══════════════════════════════════════════════════════════
 *  MQTT wire helpers
 * ══════════════════════════════════════════════════════════ */

static int enc_str(uint8_t *buf, const char *s, size_t slen)
{
    buf[0] = (slen >> 8) & 0xFF;
    buf[1] = slen & 0xFF;
    memcpy(buf + 2, s, slen);
    return (int)(2 + slen);
}

static int enc_remlen(uint8_t *buf, uint32_t n)
{
    int i = 0;
    do
    {
        uint8_t b = n & 0x7F;
        n >>= 7;
        if (n)
            b |= 0x80;
        buf[i++] = b;
    } while (n);
    return i;
}

static int read_remlen(uint32_t *out)
{
    uint32_t val = 0;
    int shift = 0;
    for (int i = 0; i < 4; i++)
    {
        uint8_t b;
        if (net_read_exact(&b, 1) != 1)
            return -1;
        val |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80))
        {
            *out = val;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

/* ── CONNECT ─────────────────────────────────────────────── */

static esp_err_t send_connect(void)
{
    const char *cid = s_broker.client_id ? s_broker.client_id : "";
    const char *user = s_broker.user ? s_broker.user : "";
    const char *pass = s_broker.password ? s_broker.password : "";
    int keepalive = s_broker.keepalive_s ? s_broker.keepalive_s : 60;

    uint8_t flags = 0x02;
    if (user && user[0])
        flags |= 0x80;
    if (pass && pass[0])
        flags |= 0x40;

    uint8_t var[10] = {0, 4, 'M', 'Q', 'T', 'T', 0x04, flags,
                       (keepalive >> 8) & 0xFF, keepalive & 0xFF};

    size_t cid_len = strlen(cid);
    size_t user_len = (user && user[0]) ? strlen(user) : 0;
    size_t pass_len = (pass && pass[0]) ? strlen(pass) : 0;

    uint8_t payload[512];
    int pos = 0;
    pos += enc_str(payload + pos, cid, cid_len);
    if (user_len)
        pos += enc_str(payload + pos, user, user_len);
    if (pass_len)
        pos += enc_str(payload + pos, pass, pass_len);

    uint32_t remlen = sizeof(var) + pos;
    uint8_t pkt[600];
    pkt[0] = 0x10;
    int rl = enc_remlen(pkt + 1, remlen);
    memcpy(pkt + 1 + rl, var, sizeof(var));
    memcpy(pkt + 1 + rl + sizeof(var), payload, pos);
    if (net_write(pkt, 1 + rl + remlen) < 0)
        return ESP_FAIL;

    uint8_t hdr[4];
    if (net_read_exact(hdr, 4) != 4)
        return ESP_FAIL;
    if (hdr[0] != 0x20 || hdr[1] != 0x02)
        return ESP_FAIL;
    if (hdr[3] != 0x00)
    {
        ESP_LOGE(TAG, "CONNACK rc=%d", hdr[3]);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CONNACK OK (session_present=%d)", hdr[2] & 1);
    return ESP_OK;
}

/* ── SUBSCRIBE ───────────────────────────────────────────── */

static esp_err_t send_subscribe(const char *topic)
{
    size_t tlen = strlen(topic);
    uint8_t body[256];
    int pos = 0;
    body[pos++] = 0x00;
    body[pos++] = 0x01;
    pos += enc_str(body + pos, topic, tlen);
    body[pos++] = 0x00;

    uint8_t pkt[270];
    pkt[0] = 0x82;
    int rl = enc_remlen(pkt + 1, pos);
    memcpy(pkt + 1 + rl, body, pos);
    if (net_write(pkt, 1 + rl + pos) < 0)
        return ESP_FAIL;

    uint8_t hdr[2];
    if (net_read_exact(hdr, 2) != 2)
        return ESP_FAIL;
    if (hdr[0] != 0x90)
        return ESP_FAIL;
    uint8_t rest[4];
    if (net_read_exact(rest, hdr[1]) != hdr[1])
        return ESP_FAIL;
    ESP_LOGI(TAG, "SUBACK OK");
    return ESP_OK;
}

/* ── PUBLISH (QoS 0) ─────────────────────────────────────── */

static esp_err_t send_publish(const char *topic, const char *payload)
{
    size_t tlen = strlen(topic);
    size_t plen = strlen(payload);
    uint32_t remlen = 2 + tlen + plen;

    uint8_t hdr[6];
    hdr[0] = 0x30;
    int rl = enc_remlen(hdr + 1, remlen);
    uint8_t topic_hdr[2] = {(tlen >> 8) & 0xFF, tlen & 0xFF};

    if (net_write(hdr, 1 + rl) < 0)
        return ESP_FAIL;
    if (net_write(topic_hdr, 2) < 0)
        return ESP_FAIL;
    if (net_write((uint8_t *)topic, tlen) < 0)
        return ESP_FAIL;
    if (net_write((uint8_t *)payload, plen) < 0)
        return ESP_FAIL;
    return ESP_OK;
}

/* ── PING ────────────────────────────────────────────────── */

static esp_err_t send_ping(void)
{
    uint8_t p[2] = {0xC0, 0x00};
    return (net_write(p, 2) == 2) ? ESP_OK : ESP_FAIL;
}

/* ── Incoming packet dispatch ────────────────────────────── */

static void check_msg_nonblocking(void)
{
    uint8_t b;
    int r;
    /* For TLS: SO_RCVTIMEO (500 ms) is set on the socket, so this
     * read returns quickly with r==0 when there is nothing incoming.
     * For plain TCP: MSG_DONTWAIT returns immediately either way.    */
    if (s_tls_handle)
        r = net_read(&b, 1); /* returns 0 on timeout, >0 on data */
    else
        r = recv(s_sock, &b, 1, MSG_DONTWAIT);

    if (r <= 0)
        return; /* 0 = timeout/no data, <0 = error — both bail */

    uint32_t remlen = 0;
    if (read_remlen(&remlen) != 0)
        return;

    uint8_t pkt_type = b & 0xF0;
    if (pkt_type == 0xD0)
        return; /* PINGRESP */

    if (pkt_type == 0x30)
    {
        uint8_t *data = malloc(remlen);
        if (!data)
            return;
        if (net_read_exact(data, remlen) != (int)remlen)
        {
            free(data);
            return;
        }
        uint16_t tlen = ((uint16_t)data[0] << 8) | data[1];
        if (tlen + 2 > remlen)
        {
            free(data);
            return;
        }
        const char *topic = (char *)(data + 2);
        const char *payload = (char *)(data + 2 + tlen);
        int plen = (int)(remlen - 2 - tlen);
        if (s_topics.on_message)
            s_topics.on_message(topic, tlen, payload, plen);
        free(data);
    }
    else
    {
        uint8_t *dummy = malloc(remlen);
        if (dummy)
        {
            net_read_exact(dummy, remlen);
            free(dummy);
        }
    }
}

/* ══════════════════════════════════════════════════════════
 *  TLS connect
 * ══════════════════════════════════════════════════════════ */

static esp_err_t tls_connect(const char *host, int port)
{
    esp_tls_cfg_t cfg = {
        .cacert_buf = (const unsigned char *)s_ca_cert,
        .cacert_bytes = s_ca_cert ? strlen(s_ca_cert) + 1 : 0,
        .clientcert_buf = (const unsigned char *)s_client_cert,
        .clientcert_bytes = s_client_cert ? strlen(s_client_cert) + 1 : 0,
        .clientkey_buf = (const unsigned char *)s_client_key,
        .clientkey_bytes = s_client_key ? strlen(s_client_key) + 1 : 0,
        .skip_common_name = false,
        .non_block = false,
        .timeout_ms = 30000,
    };
    if (!s_ca_cert)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

    s_tls_handle = esp_tls_init();
    if (!s_tls_handle)
        return ESP_FAIL;

    int ret = esp_tls_conn_new_sync(host, strlen(host), port, &cfg, s_tls_handle);
    if (ret != 1)
    {
        ESP_LOGE(TAG, "TLS connect failed: %d", ret);
        esp_tls_conn_destroy(s_tls_handle);
        s_tls_handle = NULL;
        return ESP_FAIL;
    }

    /* Set a 500 ms read timeout on the underlying socket.
     *
     * WHY: esp_tls_conn_read() calls mbedtls_ssl_read() which ultimately
     * calls recv() on the raw socket.  Without SO_RCVTIMEO, recv() blocks
     * indefinitely when there is no data — this made check_msg_nonblocking()
     * hold s_lock for 10+ seconds, starving mqtt_publish_json() and
     * triggering the mutex timeout / WDT panic every publish cycle.
     *
     * With 500 ms timeout: check_msg_nonblocking() returns quickly when
     * idle (EAGAIN), the lock is released promptly, and publish can
     * acquire it between loop ticks.                                    */
    int fd = -1;
    esp_tls_get_conn_sockfd(s_tls_handle, &fd);
    if (fd >= 0)
    {
        struct timeval tv = {.tv_sec = 0, .tv_usec = 500000}; /* 500 ms */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ESP_LOGI(TAG, "TLS rx timeout = 500 ms (fd=%d)", fd);
    }

    ESP_LOGI(TAG, "TLS connected");
    return ESP_OK;
}

static void tls_close(void)
{
    if (s_tls_handle)
    {
        esp_tls_conn_destroy(s_tls_handle);
        s_tls_handle = NULL;
    }
}

/* ══════════════════════════════════════════════════════════
 *  Connect once
 * ══════════════════════════════════════════════════════════ */

static esp_err_t connect_once(void)
{
    ESP_LOGI(TAG, "Connecting to %s:%d (ssl=%d)",
             s_broker.broker, s_broker.port, s_broker.ssl_mode);

    if (s_broker.ssl_mode != SSL_NONE)
    {
        if (tls_connect(s_broker.broker, s_broker.port) != ESP_OK)
            return ESP_FAIL;
    }
    else
    {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo *res = NULL;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", s_broker.port);
        if (getaddrinfo(s_broker.broker, port_str, &hints, &res) != 0 || !res)
            return ESP_FAIL;
        s_sock = socket(res->ai_family, res->ai_socktype, 0);
        if (s_sock < 0)
        {
            freeaddrinfo(res);
            return ESP_FAIL;
        }
        if (connect(s_sock, res->ai_addr, res->ai_addrlen) != 0)
        {
            close(s_sock);
            s_sock = -1;
            freeaddrinfo(res);
            return ESP_FAIL;
        }
        freeaddrinfo(res);
    }

    if (send_connect() != ESP_OK)
        return ESP_FAIL;
    if (send_subscribe(s_topics.sub_topic) != ESP_OK)
        return ESP_FAIL;

    s_connected = true;
    ESP_LOGI(TAG, "MQTT ready");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════
 *  Background loop task
 *
 *  This task is NOT subscribed to the Task WDT — it blocks on
 *  TLS reads which can legitimately take several seconds.
 *  Only the main task is WDT-subscribed (see app_main).
 * ══════════════════════════════════════════════════════════ */

static void mqtt_loop_task(void *arg)
{
    /* Explicitly unsubscribe this task from WDT.
     * The loop blocks inside TLS read/write which can stall
     * for several seconds — that is normal and should NOT
     * trigger a watchdog reset.                             */
    esp_task_wdt_delete(NULL);

    int ping_counter = 0;
    const int PING_EVERY = 10; /* × 1500 ms = 15 s */

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1500));

        if (!s_connected)
        {
            ESP_LOGW(TAG, "Loop: reconnecting...");
            if (s_tls_handle)
                tls_close();
            if (s_sock >= 0)
            {
                close(s_sock);
                s_sock = -1;
            }

            int backoff = 2;
            while (connect_once() != ESP_OK)
            {
                ESP_LOGW(TAG, "Reconnect failed, retry in %d s", backoff);
                vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
                if (backoff < 60)
                    backoff *= 2;
            }
            ping_counter = 0;
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        check_msg_nonblocking();
        xSemaphoreGive(s_lock);

        ping_counter++;
        if (ping_counter >= PING_EVERY)
        {
            ping_counter = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (send_ping() != ESP_OK)
            {
                ESP_LOGW(TAG, "Ping failed — reconnecting");
                s_connected = false;
            }
            else
            {
                ESP_LOGD(TAG, "Ping OK");
            }
            xSemaphoreGive(s_lock);
        }
    }
}

/* ══════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════ */

esp_err_t mqtt_manager_init(const mqtt_broker_cfg_t *broker,
                            const mqtt_topic_cfg_t *topics)
{
    s_broker = *broker;
    s_topics = *topics;
    strncpy(s_pub_topic, topics->pub_topic, sizeof(s_pub_topic) - 1);

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;

    if (broker->ssl_mode != SSL_NONE)
        load_certs(broker->ssl_mode);

    /* Initial connection — called from app_main which temporarily
     * unsubscribes itself from WDT around this call (see main.c)  */
    int backoff = 2, attempts = 0;
    while (connect_once() != ESP_OK)
    {
        attempts++;
        ESP_LOGW(TAG, "Initial connect failed (%d/5), retry in %d s", attempts, backoff);
        if (attempts >= 5)
        {
            ESP_LOGE(TAG, "MQTT failed after 5 attempts — rebooting");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
        if (backoff < 60)
            backoff *= 2;
    }

    xTaskCreate(mqtt_loop_task, "mqtt_loop", 8192, NULL, 5, NULL);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════
 *  mqtt_publish_json
 *
 *  ROOT CAUSE OF WDT PANIC:
 *
 *  The old code used xSemaphoreTake(s_lock, portMAX_DELAY).
 *  mqtt_loop_task holds s_lock while running check_msg_nonblocking()
 *  or send_ping() — both of which can block inside TLS read/write
 *  for several seconds.  The main task waited forever on the mutex,
 *  never calling esp_task_wdt_reset(), and the WDT fired.
 *
 *  FIX: try to take the mutex in short 200 ms slices, feeding the
 *  WDT between each attempt.  Total wait cap = 10 s; if the lock
 *  is still held after that something is seriously wrong and we
 *  mark the connection dead so the loop task reconnects.
 * ══════════════════════════════════════════════════════════ */

esp_err_t mqtt_publish_json(cJSON *payload, const char *topic)
{
    if (!s_connected)
    {
        ESP_LOGW(TAG, "publish: not connected");
        return ESP_FAIL;
    }

    char *json_str = cJSON_PrintUnformatted(payload);
    if (!json_str)
        return ESP_ERR_NO_MEM;

    const char *pub = topic ? topic : s_pub_topic;

    /* Try to acquire the mutex in 200 ms slices, feeding WDT each time.
     * This prevents the main task from starving the watchdog while the
     * loop task holds the lock doing TLS I/O.                          */
    bool got_lock = false;
    for (int i = 0; i < 50; i++) /* 50 × 200 ms = 10 s max */
    {
        esp_task_wdt_reset(); /* feed WDT every slice    */
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE)
        {
            got_lock = true;
            break;
        }
    }

    if (!got_lock)
    {
        ESP_LOGE(TAG, "publish: mutex timeout — forcing reconnect");
        s_connected = false;
        free(json_str);
        return ESP_FAIL;
    }

    esp_err_t err = send_publish(pub, json_str);
    xSemaphoreGive(s_lock);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "[TX] → %s", pub);
    else
    {
        ESP_LOGW(TAG, "publish failed — marking disconnected");
        s_connected = false;
    }

    free(json_str);
    return err;
}

bool mqtt_is_connected(void)
{
    return s_connected;
}