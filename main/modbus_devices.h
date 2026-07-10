#pragma once
#include "modbus_device.h"

/* The device table — add one entry per Modbus slave here (in
 * modbus_devices.c), plus its own decode_<device>.h/.c module.
 * main.c iterates this array and never needs to change when a device
 * is added, removed, or its register map changes. */
extern const modbus_device_t MODBUS_DEVICES[];
extern const size_t MODBUS_DEVICE_COUNT;