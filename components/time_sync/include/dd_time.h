#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dd_time_init(void);

// Start SNTP. Safe to call multiple times; later calls re-init.
esp_err_t dd_time_sntp_start(void);

bool      dd_time_is_synced(void);

// Wall clock unix seconds since epoch. Returns 0 if NTP hasn't synced yet.
int64_t   dd_time_now_unix(void);

// Monotonic microseconds since boot. Always usable; for back-filling timestamps
// of events that were captured before NTP sync.
int64_t   dd_time_mono_us(void);

// Convert a captured monotonic timestamp into a wall-clock unix seconds. Returns
// 0 if NTP isn't synced yet.
int64_t   dd_time_mono_to_unix(int64_t mono_us);

// SNTP server hostname. Persisted in NVS namespace "time" key "ntp_server".
// Default "ntp.aliyun.com". Setting triggers an immediate re-sync against
// the new server. Must be ≤ DD_NTP_SERVER_MAX-1 chars.
#define DD_NTP_SERVER_MAX 64
const char *dd_time_get_ntp_server(void);
esp_err_t   dd_time_set_ntp_server(const char *server);

#ifdef __cplusplus
}
#endif
