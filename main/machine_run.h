#pragma once
#include <stdbool.h>

/**
 * Device-table on_data hook (see modbus_device_t) for the power meter.
 * Called with a `const powermeter_data_t *` right after a successful
 * read. Just caches the reading — the actual gating decision lives in
 * machine_is_running().
 */
void machine_run_capture(const void *data);

/**
 * True if the machine appears to be running, based on the most recent
 * power-meter current reading vs MACHINE_RUN_CURRENT_A (config.h).
 *
 * Returns true (fail-open) if no power-meter reading has arrived yet,
 * or if the last read attempt failed — so a Modbus glitch on the power
 * meter never permanently mutes a real pressure alarm. Once at least
 * one good reading has come in, this reflects that reading until the
 * next successful one replaces it.
 */
bool machine_is_running(void);

/**
 * Bench-test override. While active, machine_is_running() ignores the
 * power meter entirely and returns `running_state` instead. Set via
 * the "force_machine_run" MQTT command (device_command.c) so you can
 * exercise the alarm/silence logic without real current flowing.
 *
 * @param active         true = override engaged, false = clear it and
 *                        go back to the real power-meter reading.
 * @param running_state  the value machine_is_running() should return
 *                        while the override is active. Ignored when
 *                        active is false.
 */
void machine_run_set_override(bool active, bool running_state);