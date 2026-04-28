#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DD_WIFI_STATE_DOWN = 0,
    DD_WIFI_STATE_AP,           // SoftAP up
    DD_WIFI_STATE_STA_CONNECTING,
    DD_WIFI_STATE_STA_GOT_IP,
} dd_wifi_state_t;

// Decides STA vs SoftAP from dd_config_boot_mode().
// Must be called after dd_config_init().
esp_err_t dd_wifi_start(void);

dd_wifi_state_t dd_wifi_state(void);
const char *dd_wifi_state_str(dd_wifi_state_t s);

// Returns IPv4 of whichever interface is currently active (STA's local IP, or
// AP gateway IP). Empty string if no interface up.
void dd_wifi_get_ip(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
