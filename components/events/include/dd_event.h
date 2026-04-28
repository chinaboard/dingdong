#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DD_EV_IN  = 0,
    DD_EV_OUT = 1,
} dd_event_type_t;

typedef enum {
    DD_SRC_BLE_AUTO    = 0,
    DD_SRC_MANUAL_WEB  = 1,
    DD_SRC_MANUAL_BTN  = 2,
} dd_event_source_t;

// Record one event. peer_addr may be NULL if unknown (e.g. manual web). For BLE
// events, pass the raw 6-byte peer address. reason is BLE disconnect reason
// for OUT events; ignored otherwise.
//
// Lightweight 10s debounce inside: same (peer, type) within 10s is dropped.
esp_err_t dd_event_record(dd_event_type_t type, dd_event_source_t src,
                          const uint8_t peer_addr[6], int reason,
                          const char *note);

const char *dd_event_type_str(dd_event_type_t t);
const char *dd_event_source_str(dd_event_source_t s);

#ifdef __cplusplus
}
#endif
