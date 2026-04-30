// Tiny header-only utilities shared across components — purely formatting,
// no state. If something here grows beyond ~5 lines or needs state, promote
// it to its own component instead of polluting this header.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include "esp_chip_info.h"

#ifdef __cplusplus
extern "C" {
#endif

// "ESP32-C6" / "ESP32-C3" / "?" — used in boot banner, /api/system/diag,
// and anywhere else we want to show what chip this firmware is running on.
static inline const char *dd_chip_model_str(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32C3: return "ESP32-C3";
    default:           return "?";
    }
}

// Format a 6-byte BLE / WiFi address as "xx:xx:xx:xx:xx:xx" (17 chars + NUL).
// Caller passes a buffer of at least 18 bytes.
static inline void dd_format_mac(const uint8_t addr[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

#ifdef __cplusplus
}
#endif
