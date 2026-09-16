#pragma once
#include <stdint.h>

/* ── Network ─────────────────────────────────────────────
 * NET_MODE: "auto" | "lan" | "wifi"
 *   auto  = try W5500 LAN first, fall back to WiFi
 *   lan   = W5500 only
 *   wifi  = WiFi only
 */
#define NET_MODE "lan"
#define WIFI_SSID ""
#define WIFI_PASS ""

/* ── Telemetry identity ────────────────────────────────── */
#define TENANT_ID "f00af313-35eb-4af0-9ce6-555eb48a6537"
#define DEVICE_ID "b3eedd10-2470-4aa4-b138-fd86233aea0e"
#define MACHINE_ID NULL /* set to a string or leave NULL */
#define LOT_ID NULL
#define FW_VERSION "1.0.0"
#define HEARTBEAT_INTERVAL_S 30 /* 30 sec */

/* ── MQTT ────────────────────────────────────────────────
 * SSL_MODE: "none" | "server" | "mutual"
 *   none   = plain MQTT (port 1883, no TLS)
 *   server = broker CA verify only         (port 8883)
 *   mutual = mTLS, client cert + key needed (port 8883)
 *
 * Cert files live in the SPIFFS partition, mounted at /spiffs
 *   /spiffs/certs/ca.pem
 *   /spiffs/certs/client.crt   (mutual only)
 *   /spiffs/certs/client.key   (mutual only)
 */
#define BROKER "bc83c2b1.ala.asia-southeast1.emqxsl.com"
#define BROKER_HOSTNAME "" /* SNI override; empty = use BROKER */
#define PORT_MQTT 1883
#define PORT_MQTTS 8883
#define MQTT_USER "gcl"
#define MQTT_PASSWORD "gcl"
#define USE_SSL true
#define SSL_MODE "server" /* "none" | "server" | "mutual" */
#define TENANT_SHORT "gcl"
#define CLIENT_ID "MCS-11-PC-ECM50"
#define SUB_TOPIC TENANT_SHORT "/" DEVICE_ID "/post"
#define PUB_TOPIC TENANT_SHORT "/devices/payload"

/* ── W5500 SPI pins ──────────────────────────────────────
 * Must match your PCB layout.
 */
#define W5500_SPI_HOST SPI2_HOST
#define W5500_PIN_SCK 12
#define W5500_PIN_MOSI 11
#define W5500_PIN_MISO 13
#define W5500_PIN_CS 47
#define W5500_PIN_RST 48
#define W5500_PIN_INT 14
#define W5500_SPI_CLOCK (5 * 1000 * 1000) /* 5 MHz */
static const uint8_t W5500_MAC[6] = {0x14, 0xc1, 0x9f, 0xc1, 0x4f, 0x54};

/* ── LAN Static IP ─────────────────────────────────────────
 * LAN_USE_STATIC_IP: false = DHCP (default), true = use the
 * static values below instead.
 */
#define LAN_USE_STATIC_IP true
#define LAN_STATIC_IP "192.168.8.183"
#define LAN_STATIC_GATEWAY "192.168.8.21"
#define LAN_STATIC_NETMASK "255.255.255.0"
#define LAN_STATIC_DNS "8.8.8.8" /* DHCP normally supplies this via option 6 */

/* ── Modbus ──────────────────────────────────────────────
 * mode: "rtu" | "tcp"
 */
#define MODBUS_MODE "rtu"
#define MODBUS_TCP_SERVER "192.168.20.5"
#define MODBUS_TCP_PORT 8080
#define MODBUS_TCP_TIMEOUT_MS 5000
#define MODBUS_UART_NUM UART_NUM_1
#define MODBUS_PIN_TX 17
#define MODBUS_PIN_RX 18
#define MODBUS_PIN_DE 19 /* RS-485 DE/RE; -1 to disable */
#define MODBUS_BAUDRATE 9600
#define MODBUS_RETRIES 5
#define MODBUS_RETRY_DELAY_MS 2000
#define MODBUS_INTER_BLOCK_MS 100

/* ── Modbus device table ───────────────────────────────────
 * The actual table — which slaves to poll and which decode_*.c module
 * reads/decodes each one — is defined in config.c. That is the one
 * file you edit to add, remove, or reconfigure a Modbus slave; this
 * header just declares it so any file including config.h can see it.
 * main.c never needs to change when a slave is added.
 *
 * To add a slave:
 *   1. Write decode_<device>.h/.c (see decode_powermeter.h/.c for the
 *      pattern: a typed read()/to_json() pair, plus a matching
 *      *_generic() wrapper pair for the table below).
 *   2. In config.c: #include "decode_<device>.h" and add a row to
 *      MODBUS_DEVICES[].
 */
#include "modbus_device.h"
extern const modbus_device_t MODBUS_DEVICES[];
extern const size_t MODBUS_DEVICE_COUNT;

/* ── Pressure alarm / relay output ────────────────────────
 * DO1/DO2 close together when the pressure sensor's on_data hook
 * (pressure_alarm.c) sees pressure <= PRESSURE_ALARM_THRESHOLD_MPA.
 * See relay_control.c's RELAY_ACTIVE_LEVEL if your relay board is
 * active-low instead of active-high.
 */
#define RELAY_PIN_DO1 15
#define RELAY_PIN_DO2 16
#define PRESSURE_ALARM_THRESHOLD_MPA 0.0
#define PRESSURE_ALARM_DEBOUNCE_S 5 /* pressure must stay <= threshold this long before tripping */
#define MACHINE_RUN_CURRENT_A 5.0   /* below this, machine considered stopped — buzzer forced off */

/* ── Watchdog / timing ───────────────────────────────────
 * WDT_TIMEOUT_S is registered with the ESP Task WDT.
 * The main task calls esp_task_wdt_reset() every loop tick,
 * matching the MicroPython machine.WDT(timeout=...) / wdt.feed() pattern.
 */
#define WDT_TIMEOUT_S 20 /* hardware watchdog (seconds) */
#define PUBLISH_INTERVAL_S 30
#define NET_CHECK_INTERVAL_S 10
#define NET_FAIL_RESET_COUNT 3
#define MQTT_FAIL_RESET_COUNT 5
