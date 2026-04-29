#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DD_BOOT_FIRST_RUN = 0,  // No admin password set → SoftAP first-run wizard
    DD_BOOT_NO_WIFI,        // Admin set, but no WiFi creds → SoftAP reconfig
    DD_BOOT_NORMAL,         // Admin + WiFi present → STA mode
} dd_boot_mode_t;

#define DD_WIFI_SSID_MAX 32
#define DD_WIFI_PASS_MAX 64

esp_err_t dd_config_init(void);

dd_boot_mode_t dd_config_boot_mode(void);
const char *dd_boot_mode_str(dd_boot_mode_t m);

bool      dd_config_has_admin(void);
esp_err_t dd_config_set_admin_password(const char *plaintext);
esp_err_t dd_config_verify_admin_password(const char *plaintext, bool *out_ok);

// Raw admin credential blob accessors for backup/restore. Salt is 16 bytes,
// hash is 32 bytes (PBKDF2-SHA256 of the password). NEVER expose these to
// untrusted clients.
#define DD_ADMIN_SALT_LEN 16
#define DD_ADMIN_HASH_LEN 32
esp_err_t dd_config_get_admin_blob(uint8_t salt[DD_ADMIN_SALT_LEN],
                                    uint8_t hash[DD_ADMIN_HASH_LEN]);
esp_err_t dd_config_set_admin_blob(const uint8_t salt[DD_ADMIN_SALT_LEN],
                                    const uint8_t hash[DD_ADMIN_HASH_LEN]);

bool      dd_config_has_wifi(void);
esp_err_t dd_config_set_wifi(const char *ssid, const char *password);
esp_err_t dd_config_get_wifi(char *ssid_out, size_t ssid_cap,
                              char *pass_out, size_t pass_cap);

// BLE advertising name (visible in iPhone Bluetooth settings).
// Default is "dingdong-XXXX" with last 2 BT MAC bytes if not set.
#define DD_DEVICE_NAME_MAX 20
esp_err_t dd_config_get_device_name(char *out, size_t cap);
esp_err_t dd_config_set_device_name(const char *name);

esp_err_t dd_config_factory_reset(void);

// Reliability counters persisted across reboots.
typedef struct {
    uint32_t boot_count;       // increments at every boot
    uint64_t total_uptime_s;   // accumulates over device lifetime
    uint32_t last_uptime_s;    // last session's uptime when it died
} dd_metrics_t;

esp_err_t dd_metrics_load(dd_metrics_t *out);
esp_err_t dd_metrics_record_boot(void);   // increments boot_count, returns new
esp_err_t dd_metrics_save_uptime(uint32_t seconds);  // total += seconds, last = seconds
esp_err_t dd_metrics_reset(void);          // zeros boot_count + total/last uptime

#ifdef __cplusplus
}
#endif
