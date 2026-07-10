#pragma once

#include <time.h>
/**
 * ntp_sync.h — NTP synchronisation helper
 *
 * Call after network is up and WDT is initialised.
 * Writes the synced boot timestamp into *boot_time_out on success.
 * Falls back gracefully (logs a warning) if sync fails within 30 s.
 */
void sync_ntp(time_t *boot_time_out);