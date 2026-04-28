#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mini captive-portal DNS server. Resolves every A query to `gateway_ip`
// (typically the SoftAP gateway, e.g. 10.10.10.1) so iOS / Android / Windows
// connectivity probes hit our HTTP server and the OS pops up the in-network
// browser pointed at the setup page.
//
// Idempotent: a second start with the same IP is a no-op. Stop is safe to
// call when not running. Intended to run only in SoftAP boot modes.
esp_err_t dd_captive_start(uint32_t gateway_ip);
esp_err_t dd_captive_stop(void);

#ifdef __cplusplus
}
#endif
