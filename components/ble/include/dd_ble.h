#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dd_ble_start(void);

// Change BLE adv name at runtime. Restarts the HID advertising instance with
// new name in adv data. NimBLE GAP device name char also updated. Already-
// bonded iPhones will continue to show the OLD name in their bluetooth list
// (iOS caches it); user must "Forget This Device" + re-pair to see the new
// name on already-bonded iPhones.
esp_err_t dd_ble_set_device_name(const char *name);

bool        dd_ble_is_advertising(void);
const char *dd_ble_device_name(void);

// Pairing window. While active, new iPhones can pair via Numeric Comparison.
// SM is configured with IO cap = KEYBOARD_DISPLAY + sm_mitm = 1, so pairing
// with iOS (DisplayYesNo IO cap) selects Numeric Comparison: a 6-digit number
// is shown on both iPhone and the Web UI; user verifies they match and
// confirms on both sides.
//
// Outside the window, the security_initiate request is suppressed so strangers
// can connect but won't be pushed into pairing. ttl_ms is clamped to
// [5000, 120000].
esp_err_t dd_ble_pairing_start(int64_t ttl_ms);
void      dd_ble_pairing_cancel(void);
bool      dd_ble_pairing_active(void);
int64_t   dd_ble_pairing_remaining_ms(void);

// Numeric Comparison: returns the 6-digit number SM derived for the in-flight
// pairing, or 0 if no pairing is currently awaiting confirmation. UI polls
// this and shows it next to whatever iPhone is displaying.
uint32_t  dd_ble_pairing_numcmp(void);

// User-side confirmation for Numeric Comparison. Pass true if the Web UI
// number matches the iPhone number; false to reject.
esp_err_t dd_ble_pairing_confirm(bool accept);

// Bond management. Returns count of bonded peers; up to `cap` filled into
// `out_addrs` (each 6 bytes). Caller-allocated buffer.
int       dd_ble_bond_list(uint8_t (*out_addrs)[6], int cap);

// Delete one bond (and its LTK/IRK in NVS) so the iPhone can no longer
// auto-reconnect. Returns ESP_OK on success, ESP_ERR_NOT_FOUND otherwise.
esp_err_t dd_ble_bond_revoke(const uint8_t addr[6]);

// Is this address currently in the bond store?
bool      dd_ble_bond_exists(const uint8_t addr[6]);

// Forget what IN events we've already emitted for currently-present peers.
// The next presence tick will re-emit IN for everyone still in proximity.
// Called by /api/events/wipe so a wipe immediately re-populates today's view
// instead of waiting for the next OUT→IN transition.
void      dd_ble_presence_reset_events(void);

#ifdef __cplusplus
}
#endif
