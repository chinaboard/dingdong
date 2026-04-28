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
// Idempotent: a second start with a different IP just updates the answer in
// place. The task runs for the entire SoftAP lifetime (until reboot); there
// is no stop function — SoftAP modes never transition to STA without a
// reboot, so the asymmetry isn't paid for.
esp_err_t dd_captive_start(uint32_t gateway_ip);

#ifdef __cplusplus
}
#endif
