#pragma once
#include <stdbool.h>

/**
 * Parses an incoming JSON payload (the raw MQTT message body from
 * SUB_TOPIC) looking for a "cmd" field and executes the matching
 * action. Unlike device_config_apply_json(), nothing here is
 * persisted to NVS — commands are transient by design.
 *
 * Recognised commands so far:
 *   { "cmd": "silence_alarm", "duration_sec": 300 }              // duration optional, defaults to 300s
 *   { "cmd": "force_machine_run", "enabled": true, "running": true }
 *       // bench-test override for machine_is_running(); "running" optional, defaults to true.
 *       // Send { "cmd": "force_machine_run", "enabled": false } to clear it and go back to the
 *       // real power-meter reading.
 *
 * @return true if a recognised command was executed.
 */
bool device_command_execute(const char *payload, int payload_len);

/**
 * True if the buzzer is currently inside a remote-silence window set
 * by a "silence_alarm" command. pressure_alarm_check() calls this to
 * force the buzzer off even while the underlying pressure condition
 * is still tripped. Auto-clears (returns false) once the window
 * elapses — a real fault can't be silenced forever by one command.
 */
bool device_command_alarm_silenced(void);