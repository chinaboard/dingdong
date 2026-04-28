#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize on-board WS2812 status LED + start a 100 ms tick that polls
// dd_wifi/dd_time/dd_ble state and updates colour/animation.
//
// No-op (and returns ESP_OK) when DD_LED_ENABLE=0 at build time.
esp_err_t dd_led_init(void);

// Runtime on/off (persisted in NVS namespace "led"). When disabled the tick
// keeps running but always pushes black. Default is enabled. Both calls are
// no-ops / return defaults when DD_LED_ENABLE=0 at build time.
esp_err_t dd_led_set_enabled(bool enabled);
bool      dd_led_is_enabled(void);

#ifdef __cplusplus
}
#endif

