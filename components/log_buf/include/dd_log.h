#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hooks ESP_LOG output into a ring buffer (forwarding to UART/USB-JTAG too).
// Call once, very early in app_main — before other components log anything
// you might want to keep. Capacity below is the on-device retention.
#define DD_LOG_BUF_BYTES 65536

esp_err_t dd_log_init(void);

// Copies up to `cap` of the most recent buffered bytes into `out`. Returns
// number of bytes written (NOT NUL-terminated; caller should add if needed).
// If `cap` >= buffer size, returns the entire buffer contents.
size_t dd_log_read_tail(char *out, size_t cap);

// Wipes the ring buffer back to empty. Forwarded ESP_LOG output to the UART
// is not affected — only the in-RAM capture used by /api/system/logs.
void dd_log_clear(void);

#ifdef __cplusplus
}
#endif
