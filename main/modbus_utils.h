#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/uart.h"

/* ── Modbus RTU master handle ──────────────────────────── */
typedef struct {
    uart_port_t uart;
    int         pin_de;        /* DE/RE GPIO; -1 if unused */
    uint32_t    baudrate;
    int         timeout_ms;
} mb_rtu_t;

/* ── Modbus TCP master handle ──────────────────────────── */
typedef struct {
    int         sock;          /* BSD socket fd */
    uint16_t    trans_id;
} mb_tcp_t;

/* ── Unified handle ────────────────────────────────────── */
typedef enum { MB_MODE_RTU, MB_MODE_TCP } mb_mode_t;

typedef struct {
    mb_mode_t mode;
    union {
        mb_rtu_t rtu;
        mb_tcp_t tcp;
    };
} modbus_t;

/**
 * Initialise a Modbus RTU master over RS-485 UART.
 * Retries up to `retries` times with `delay_ms` between attempts.
 */
esp_err_t modbus_rtu_init(modbus_t *mb,
                           uart_port_t uart_num,
                           int pin_tx, int pin_rx, int pin_de,
                           uint32_t baudrate,
                           int timeout_ms,
                           int retries, int delay_ms);

/**
 * Connect a Modbus TCP master to a server.
 * Retries up to `retries` times with `delay_ms` between attempts.
 */
esp_err_t modbus_tcp_connect(modbus_t *mb,
                              const char *server, uint16_t port,
                              int timeout_ms,
                              int retries, int delay_ms);

/**
 * Read holding registers.
 * @param out   Buffer of at least `count` int16_t values
 * Returns ESP_OK on success.
 */
esp_err_t modbus_read_holding(modbus_t *mb,
                               uint8_t  slave_addr,
                               uint16_t reg_start,
                               uint16_t count,
                               int16_t *out);

/** Close and free resources. */
void modbus_close(modbus_t *mb);
