#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum
{
    NET_IFACE_NONE,
    NET_IFACE_LAN,
    NET_IFACE_WIFI,
} net_iface_t;

/**
 * Connect to the network, mirroring connect_network() from net_utils.py.
 * mode: "auto" | "lan" | "wifi"
 * Reboots the MCU if no interface comes up.
 */
void net_connect(const char *mode, const char *wifi_ssid, const char *wifi_pass);

/**
 * Check current link state.
 * Returns the active interface, and (if WiFi) writes RSSI into *rssi_out.
 */
net_iface_t net_check(int *rssi_out);

/** Reboot with a log message — mirrors hard_reset() in Python. */
void hard_reset(const char *reason);

bool test_internet(void);
