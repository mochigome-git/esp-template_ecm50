#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "modbus_utils.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"

static const char *TAG = "modbus";

/* ══════════════════════════════════════════════════════════
 *  CRC-16 (Modbus RTU)  — mirrors _crc16() in Python
 * ══════════════════════════════════════════════════════════ */
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
        {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

/* ══════════════════════════════════════════════════════════
 *  RTU helpers
 * ══════════════════════════════════════════════════════════ */

static void rtu_set_de(int pin_de, int level)
{
    if (pin_de >= 0)
        gpio_set_level(pin_de, level);
}

/**
 * Build a read-holding-registers RTU frame into `buf`.
 * Returns frame length.
 */
static size_t rtu_build_request(uint8_t *buf,
                                uint8_t slave_addr,
                                uint16_t reg, uint16_t count)
{
    buf[0] = slave_addr;
    buf[1] = 0x03; /* READ_HOLDING_REGISTERS */
    buf[2] = (reg >> 8) & 0xFF;
    buf[3] = (reg) & 0xFF;
    buf[4] = (count >> 8) & 0xFF;
    buf[5] = (count) & 0xFF;
    uint16_t crc = crc16(buf, 6);
    buf[6] = crc & 0xFF; /* CRC low byte first (little-endian) */
    buf[7] = (crc >> 8) & 0xFF;
    return 8;
}

/**
 * Parse a read-holding-registers RTU response.
 * Mirrors _parse_rtu_response() in Python.
 */
static esp_err_t rtu_parse_response(const uint8_t *raw, size_t raw_len,
                                    uint8_t slave_addr, uint16_t count,
                                    int16_t *out)
{
    size_t min_len = 5 + count * 2;
    if (raw_len < min_len)
    {
        ESP_LOGE(TAG, "RTU response too short: %d bytes (need %d)", raw_len, min_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* CRC check — last two bytes, little-endian */
    uint16_t crc_recv = (uint16_t)raw[raw_len - 2] | ((uint16_t)raw[raw_len - 1] << 8);
    uint16_t crc_calc = crc16(raw, raw_len - 2);
    if (crc_recv != crc_calc)
    {
        ESP_LOGE(TAG, "RTU CRC mismatch: got 0x%04x expected 0x%04x", crc_recv, crc_calc);
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t addr = raw[0];
    uint8_t func = raw[1];
    uint8_t byte_cnt = raw[2];

    if (addr != slave_addr)
    {
        ESP_LOGE(TAG, "RTU slave addr mismatch: got %d expected %d", addr, slave_addr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (func == (0x03 | 0x80))
    {
        ESP_LOGE(TAG, "RTU exception code: 0x%02x", raw[2]);
        return ESP_FAIL;
    }
    if (func != 0x03)
    {
        ESP_LOGE(TAG, "RTU unexpected function code: 0x%02x", func);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (byte_cnt != count * 2)
    {
        ESP_LOGE(TAG, "RTU byte count mismatch: got %d expected %d", byte_cnt, count * 2);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Unpack big-endian int16 values */
    for (int i = 0; i < count; i++)
    {
        uint16_t raw_val = ((uint16_t)raw[3 + i * 2] << 8) | raw[3 + i * 2 + 1];
        out[i] = (int16_t)raw_val;
    }
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════
 *  RTU public
 * ══════════════════════════════════════════════════════════ */

esp_err_t modbus_rtu_init(modbus_t *mb,
                          uart_port_t uart_num,
                          int pin_tx, int pin_rx, int pin_de,
                          uint32_t baudrate,
                          int timeout_ms,
                          int retries, int delay_ms)
{
    for (int attempt = 1; attempt <= retries; attempt++)
    {
        ESP_LOGI(TAG, "[RTU] Initialising (%d/%d)...", attempt, retries);

        uart_config_t cfg = {
            .baud_rate = (int)baudrate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        };

        esp_err_t err = uart_param_config(uart_num, &cfg);
        if (err != ESP_OK)
            goto retry;

        err = uart_set_pin(uart_num, pin_tx, pin_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK)
            goto retry;

        /* RS-485 DE/RE pin as GPIO output */
        if (pin_de >= 0)
        {
            gpio_config_t io = {
                .pin_bit_mask = 1ULL << pin_de,
                .mode = GPIO_MODE_OUTPUT,
            };
            gpio_config(&io);
            gpio_set_level(pin_de, 0);
        }

        err = uart_driver_install(uart_num, 256, 0, 0, NULL, 0);
        if (err != ESP_OK)
            goto retry;

        mb->mode = MB_MODE_RTU;
        mb->rtu.uart = uart_num;
        mb->rtu.pin_de = pin_de;
        mb->rtu.baudrate = baudrate;
        mb->rtu.timeout_ms = timeout_ms;
        ESP_LOGI(TAG, "[RTU] Ready on UART%d at %lu baud", uart_num, baudrate);
        return ESP_OK;

    retry:
        ESP_LOGW(TAG, "[RTU] Failed: %s", esp_err_to_name(err));
        if (attempt < retries)
        {
            ESP_LOGI(TAG, "[RTU] Retrying in %d ms...", delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return ESP_FAIL;
}

/* ══════════════════════════════════════════════════════════
 *  TCP public
 * ══════════════════════════════════════════════════════════ */

esp_err_t modbus_tcp_connect(modbus_t *mb,
                             const char *server, uint16_t port,
                             int timeout_ms,
                             int retries, int delay_ms)
{
    for (int attempt = 1; attempt <= retries; attempt++)
    {
        ESP_LOGI(TAG, "[TCP] Connecting %s:%d (%d/%d)...", server, port, attempt, retries);

        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo *res = NULL;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", port);

        if (getaddrinfo(server, port_str, &hints, &res) != 0 || !res)
        {
            ESP_LOGW(TAG, "[TCP] DNS failed");
            goto retry;
        }

        int sock = socket(res->ai_family, res->ai_socktype, 0);
        if (sock < 0)
        {
            freeaddrinfo(res);
            goto retry;
        }

        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0)
        {
            ESP_LOGW(TAG, "[TCP] connect() failed: %d", errno);
            close(sock);
            freeaddrinfo(res);
            goto retry;
        }
        freeaddrinfo(res);

        mb->mode = MB_MODE_TCP;
        mb->tcp.sock = sock;
        mb->tcp.trans_id = 0;
        ESP_LOGI(TAG, "[TCP] Connected to %s:%d", server, port);
        return ESP_OK;

    retry:
        if (attempt < retries)
        {
            ESP_LOGI(TAG, "[TCP] Retrying in %d ms...", delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return ESP_FAIL;
}

/* ══════════════════════════════════════════════════════════
 *  Unified read_holding
 * ══════════════════════════════════════════════════════════ */

static esp_err_t rtu_read_holding(modbus_t *mb,
                                  uint8_t slave_addr,
                                  uint16_t reg, uint16_t count,
                                  int16_t *out)
{
    mb_rtu_t *r = &mb->rtu;

    uint8_t frame[8];
    size_t flen = rtu_build_request(frame, slave_addr, reg, count);

    /* Flush RX */
    uart_flush(r->uart);

    /* Assert DE → transmit → release DE */
    rtu_set_de(r->pin_de, 1);
    uart_write_bytes(r->uart, frame, flen);
    /* Wait for UART TX FIFO to drain: bits/byte * bytes / baud + 2 ms guard */
    uint32_t tx_ms = ((uint32_t)flen * 11 * 1000) / r->baudrate + 2;
    vTaskDelay(pdMS_TO_TICKS(tx_ms));
    rtu_set_de(r->pin_de, 0);

    /* Read response */
    size_t expect = 5 + count * 2;
    uint8_t buf[256];
    int got = uart_read_bytes(r->uart, buf, expect, pdMS_TO_TICKS(r->timeout_ms));
    if (got < (int)expect)
    {
        ESP_LOGW(TAG, "[RTU] Timeout: got %d/%d bytes", got, expect);
        return ESP_ERR_TIMEOUT;
    }

    return rtu_parse_response(buf, got, slave_addr, count, out);
}

static esp_err_t tcp_read_holding(modbus_t *mb,
                                  uint8_t slave_addr,
                                  uint16_t reg, uint16_t count,
                                  int16_t *out)
{
    mb_tcp_t *t = &mb->tcp;
    t->trans_id++;

    /* Build MBAP + PDU — matches uModBusTCP._create_mbap_hdr / read_holding_registers */
    uint8_t req[12];
    uint16_t pdu_len = 6;               /* func + reg(2) + count(2) = 5, but MBAP length field = PDU+1 */
    req[0] = (t->trans_id >> 8) & 0xFF; /* transaction id */
    req[1] = t->trans_id & 0xFF;
    req[2] = 0x00; /* protocol id */
    req[3] = 0x00;
    req[4] = 0x00; /* length MSB */
    req[5] = 0x06; /* length: unit_id(1) + func(1) + reg(2) + cnt(2) */
    req[6] = slave_addr;
    req[7] = 0x03; /* READ_HOLDING_REGISTERS */
    req[8] = (reg >> 8) & 0xFF;
    req[9] = reg & 0xFF;
    req[10] = (count >> 8) & 0xFF;
    req[11] = count & 0xFF;

    if (send(t->sock, req, sizeof(req), 0) != sizeof(req))
    {
        ESP_LOGW(TAG, "[TCP] send failed: %d", errno);
        return ESP_FAIL;
    }

    /* Receive MBAP (7 bytes) + PDU */
    uint8_t hdr[9]; /* MBAP(7) + func(1) + byte_count(1) */
    int got = recv(t->sock, hdr, sizeof(hdr), MSG_WAITALL);
    if (got < (int)sizeof(hdr))
    {
        ESP_LOGW(TAG, "[TCP] header recv failed: %d", got);
        return ESP_ERR_TIMEOUT;
    }

    /* Validate transaction id */
    uint16_t resp_tid = ((uint16_t)hdr[0] << 8) | hdr[1];
    if (resp_tid != t->trans_id)
    {
        ESP_LOGW(TAG, "[TCP] transaction id mismatch");
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t resp_func = hdr[7];
    uint8_t resp_bcnt = hdr[8];

    if (resp_func == (0x03 | 0x80))
    {
        ESP_LOGE(TAG, "[TCP] exception: 0x%02x", resp_bcnt);
        return ESP_FAIL;
    }
    if (resp_func != 0x03 || resp_bcnt != count * 2)
    {
        ESP_LOGE(TAG, "[TCP] unexpected func/bytecount");
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t data[250];
    got = recv(t->sock, data, resp_bcnt, MSG_WAITALL);
    if (got != resp_bcnt)
    {
        ESP_LOGW(TAG, "[TCP] data recv short: %d/%d", got, resp_bcnt);
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < count; i++)
    {
        uint16_t raw_val = ((uint16_t)data[i * 2] << 8) | data[i * 2 + 1];
        out[i] = (int16_t)raw_val;
    }
    return ESP_OK;
}

esp_err_t modbus_read_holding(modbus_t *mb,
                              uint8_t slave_addr,
                              uint16_t reg_start,
                              uint16_t count,
                              int16_t *out)
{
    if (mb->mode == MB_MODE_RTU)
        return rtu_read_holding(mb, slave_addr, reg_start, count, out);
    else
        return tcp_read_holding(mb, slave_addr, reg_start, count, out);
}

void modbus_close(modbus_t *mb)
{
    if (mb->mode == MB_MODE_TCP && mb->tcp.sock >= 0)
    {
        close(mb->tcp.sock);
        mb->tcp.sock = -1;
    }
    else if (mb->mode == MB_MODE_RTU)
    {
        uart_driver_delete(mb->rtu.uart);
    }
}
