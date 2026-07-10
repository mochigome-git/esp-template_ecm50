# esp32s3_gateway — ESP-IDF C Port

Full migration of the MicroPython gateway project to ESP-IDF (C).
**Identical runtime behaviour** — same watchdog, same publish cadence, same
Modbus logic, same MQTT payload shape, same TLS cert loading.

---

## Project layout

```
esp32s3_gateway/
├── CMakeLists.txt          Root build file
├── sdkconfig.defaults      Pre-tuned IDF Kconfig
├── partitions.csv          App (1.5 MB) + SPIFFS (256 KB) partition table
├── certs/
│   ├── README.md           How to generate & flash certs
│   ├── ca.pem              ← you add this
│   ├── client.crt          ← you add this (mutual TLS only)
│   └── client.key          ← you add this (mutual TLS only)
└── main/
    ├── CMakeLists.txt
    ├── config.h            ← ALL tunables live here (replaces config.py)
    ├── main.c              Boot sequence + main loop
    ├── decode_weather.c/h  Register → engineering-unit decoder
    ├── modbus_utils.c/h    Modbus RTU (RS-485) + TCP master
    ├── net_utils.c/h       W5500 LAN + WiFi, auto-fallback
    └── mqtt_manager.c/h    esp-mqtt wrapper, SPIFFS cert loader
```

---

## Quick start

### 1. Prerequisites

```bash
# ESP-IDF v5.2+ required (W5500 driver + new MQTT API)
. $IDF_PATH/export.sh
idf.py --version   # should print 5.x
```

### 2. Configure

Edit **`main/config.h`** — every tuneable is there:

| Symbol                  | Default         | Replaces                |
| ----------------------- | --------------- | ----------------------- |
| `NET_MODE`              | `"auto"`        | `NET_MODE` in config.py |
| `WIFI_SSID / WIFI_PASS` | see file        | same                    |
| `BROKER`                | `172.31.25.174` | same                    |
| `SSL_MODE`              | `"mutual"`      | same                    |
| `MODBUS_MODE`           | `"rtu"`         | `MODBUS_CFG["mode"]`    |
| `PUBLISH_INTERVAL_S`    | 30              | same                    |
| `WDT_TIMEOUT_S`         | 8               | `WDT_TIMEOUT_MS / 1000` |

### 3. Place certificates

See `certs/README.md`. For plain MQTT set `USE_SSL false` in `config.h`.

### 4. Flash certs to SPIFFS

```bash
# Generate SPIFFS image from the certs/ folder
python $IDF_PATH/components/spiffs/spiffsgen.py 262144 certs spiffs.bin

# Find the SPIFFS partition offset
idf.py partition-table   # look for "spiffs" row

# Flash certs (replace offset and port as appropriate)
esptool.py --port /dev/ttyUSB0 write_flash 0x310000 spiffs.bin
```

### 5. Build & flash firmware

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## Behaviour mapping (MicroPython → C)

| MicroPython                               | C equivalent                                            |
| ----------------------------------------- | ------------------------------------------------------- |
| `machine.WDT(timeout=8000)`               | `esp_task_wdt_reconfigure()` + `esp_task_wdt_add(NULL)` |
| `wdt.feed()`                              | `esp_task_wdt_reset()` — called every loop tick         |
| `ntptime.settime()`                       | `sntp_init()` + poll                                    |
| `network.LAN(phy_type=PHY_W5500, ...)`    | `esp_eth_mac_new_w5500()` + `esp_eth_phy_new_w5500()`   |
| `network.WLAN(STA_IF)`                    | `esp_wifi_init()` + `esp_wifi_start()`                  |
| `machine.UART(...)`                       | `uart_param_config()` + `uart_driver_install()`         |
| `machine.Pin(de, OUT)`                    | `gpio_config()` + `gpio_set_level()`                    |
| `time.time() - _boot_time`                | `time(NULL) - s_boot_time`                              |
| `machine.reset()`                         | `esp_restart()`                                         |
| `ujson.dumps()`                           | `cJSON_PrintUnformatted()`                              |
| `_thread.start_new_thread()`              | MQTT handled internally by esp-mqtt task                |
| `ssl.wrap_socket(key, cert, cadata, ...)` | cert strings passed to `esp_mqtt_client_config_t`       |

---

## Adding more slaves

1. Add a decoder file `decode_xxx.c/h` (copy `decode_weather.c` as template).
2. Add the register block constants to `config.h`.
3. In `main.c`, call `modbus_read_holding()` for each new block, then
   `decode_xxx()`, then add the result to `build_payload()`.
4. Add the new `.c` file to `main/CMakeLists.txt` under `SRCS`.

---

## Watchdog behaviour

The ESP Task WDT is configured to **8 seconds** (matching `WDT_TIMEOUT_MS=8000`
in the original MicroPython). `esp_task_wdt_reset()` is called once per
second at the top of the main loop. If the loop blocks for >8 s the chip
panics and reboots — identical to the MicroPython WDT.

`CONFIG_ESP_TASK_WDT_PANIC=y` in `sdkconfig.defaults` ensures a full panic
(backtrace + reboot) rather than a silent reset, which aids debugging.

---

## TLS / cert notes

| `SSL_MODE` | Cert files needed                    | Broker port |
| ---------- | ------------------------------------ | ----------- |
| `"none"`   | —                                    | 1883        |
| `"server"` | `ca.pem`                             | 8883        |
| `"mutual"` | `ca.pem`, `client.crt`, `client.key` | 8883        |

The cert files are loaded from the SPIFFS partition at `/spiffs/certs/` every
boot. Missing files produce a warning but do not crash — matching the
`try/except OSError` in `ssl_utils.py`.

## Read Mac address

```
esptool.py --port /dev/ttyUSB0 read_mac
```
