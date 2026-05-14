#include "dd_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

static const char *TAG = "config";

#define NS              "dingdong"
#define KEY_ADMIN_SALT  "admin_salt"
#define KEY_ADMIN_HASH  "admin_hash"
#define KEY_WIFI_SSID   "wifi_ssid"
#define KEY_WIFI_PASS   "wifi_pass"
#define KEY_DEVICE_NAME "dev_name"

#define ADMIN_SALT_LEN  16
#define ADMIN_HASH_LEN  32
#define PBKDF2_ITERS    20000

esp_err_t dd_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)ps);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t pbkdf2(const char *password, const uint8_t *salt, size_t salt_len,
                        uint8_t *out, size_t out_len)
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st;
    esp_err_t err = ESP_FAIL;

    st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st != PSA_SUCCESS) goto out;

    st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, PBKDF2_ITERS);
    if (st != PSA_SUCCESS) goto out;

    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                        salt, salt_len);
    if (st != PSA_SUCCESS) goto out;

    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                        (const uint8_t *)password, strlen(password));
    if (st != PSA_SUCCESS) goto out;

    st = psa_key_derivation_output_bytes(&op, out, out_len);
    if (st == PSA_SUCCESS) err = ESP_OK;

out:
    psa_key_derivation_abort(&op);
    if (err != ESP_OK) ESP_LOGE(TAG, "pbkdf2 failed: psa=%ld", (long)st);
    return err;
}

bool dd_config_has_admin(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 0;
    esp_err_t err = nvs_get_blob(h, KEY_ADMIN_HASH, NULL, &sz);
    nvs_close(h);
    return err == ESP_OK && sz == ADMIN_HASH_LEN;
}

esp_err_t dd_config_set_admin_password(const char *plaintext)
{
    if (!plaintext || strlen(plaintext) == 0) return ESP_ERR_INVALID_ARG;

    uint8_t salt[ADMIN_SALT_LEN];
    uint8_t hash[ADMIN_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));

    esp_err_t err = pbkdf2(plaintext, salt, sizeof(salt), hash, sizeof(hash));
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, KEY_ADMIN_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(h, KEY_ADMIN_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) ESP_LOGI(TAG, "admin password set");
    return err;
}

esp_err_t dd_config_verify_admin_password(const char *plaintext, bool *out_ok)
{
    if (!plaintext || !out_ok) return ESP_ERR_INVALID_ARG;
    *out_ok = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    uint8_t salt[ADMIN_SALT_LEN];
    uint8_t want[ADMIN_HASH_LEN];
    size_t sz;

    sz = sizeof(salt);
    err = nvs_get_blob(h, KEY_ADMIN_SALT, salt, &sz);
    if (err != ESP_OK || sz != ADMIN_SALT_LEN) { nvs_close(h); return ESP_ERR_NOT_FOUND; }

    sz = sizeof(want);
    err = nvs_get_blob(h, KEY_ADMIN_HASH, want, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != ADMIN_HASH_LEN) return ESP_ERR_NOT_FOUND;

    uint8_t got[ADMIN_HASH_LEN];
    err = pbkdf2(plaintext, salt, sizeof(salt), got, sizeof(got));
    if (err != ESP_OK) return err;

    // constant-time compare
    uint8_t diff = 0;
    for (size_t i = 0; i < ADMIN_HASH_LEN; ++i) diff |= want[i] ^ got[i];
    *out_ok = (diff == 0);
    return ESP_OK;
}

esp_err_t dd_config_get_admin_blob(uint8_t salt[DD_ADMIN_SALT_LEN],
                                    uint8_t hash[DD_ADMIN_HASH_LEN])
{
    if (!salt || !hash) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = ADMIN_SALT_LEN;
    err = nvs_get_blob(h, KEY_ADMIN_SALT, salt, &sz);
    if (err != ESP_OK || sz != ADMIN_SALT_LEN) { nvs_close(h); return ESP_ERR_NOT_FOUND; }

    sz = ADMIN_HASH_LEN;
    err = nvs_get_blob(h, KEY_ADMIN_HASH, hash, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != ADMIN_HASH_LEN) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t dd_config_set_admin_blob(const uint8_t salt[DD_ADMIN_SALT_LEN],
                                    const uint8_t hash[DD_ADMIN_HASH_LEN])
{
    if (!salt || !hash) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, KEY_ADMIN_SALT, salt, ADMIN_SALT_LEN);
    if (err == ESP_OK) err = nvs_set_blob(h, KEY_ADMIN_HASH, hash, ADMIN_HASH_LEN);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "admin blob restored");
    return err;
}

bool dd_config_has_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 0;
    esp_err_t err = nvs_get_str(h, KEY_WIFI_SSID, NULL, &sz);
    nvs_close(h);
    return err == ESP_OK && sz > 1;  // sz includes NUL
}

esp_err_t dd_config_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) >= DD_WIFI_SSID_MAX)
        return ESP_ERR_INVALID_ARG;
    if (!password) password = "";
    if (strlen(password) >= DD_WIFI_PASS_MAX) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_WIFI_PASS, password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) ESP_LOGI(TAG, "wifi creds saved (ssid=%s)", ssid);
    return err;
}

esp_err_t dd_config_get_wifi(char *ssid_out, size_t ssid_cap,
                              char *pass_out, size_t pass_cap)
{
    if (!ssid_out || ssid_cap == 0 || !pass_out || pass_cap == 0)
        return ESP_ERR_INVALID_ARG;

    // Debug override: hardcode WiFi creds at build time so a blank board
    // boots straight into STA mode without going through the SoftAP setup
    // wizard. Used for remote-board debugging only — never in CI / release.
    //   make build DEBUG_WIFI_SSID='"IoToI"' DEBUG_WIFI_PASS='"54383845"'
#ifdef DEBUG_WIFI_SSID
    strncpy(ssid_out, DEBUG_WIFI_SSID, ssid_cap - 1);
    ssid_out[ssid_cap - 1] = '\0';
#ifdef DEBUG_WIFI_PASS
    strncpy(pass_out, DEBUG_WIFI_PASS, pass_cap - 1);
    pass_out[pass_cap - 1] = '\0';
#else
    pass_out[0] = '\0';
#endif
    return ESP_OK;
#endif

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t sz = ssid_cap;
    err = nvs_get_str(h, KEY_WIFI_SSID, ssid_out, &sz);
    if (err != ESP_OK) { nvs_close(h); return err; }

    sz = pass_cap;
    err = nvs_get_str(h, KEY_WIFI_PASS, pass_out, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pass_out[0] = '\0';
        err = ESP_OK;
    }
    return err;
}

dd_boot_mode_t dd_config_boot_mode(void)
{
    // Debug override: with hardcoded creds we always boot NORMAL (STA),
    // skipping the admin-password gate and the SoftAP setup wizard.
#ifdef DEBUG_WIFI_SSID
    return DD_BOOT_NORMAL;
#endif
    if (!dd_config_has_admin()) return DD_BOOT_FIRST_RUN;
    if (!dd_config_has_wifi())  return DD_BOOT_NO_WIFI;
    return DD_BOOT_NORMAL;
}

const char *dd_boot_mode_str(dd_boot_mode_t m)
{
    switch (m) {
    case DD_BOOT_FIRST_RUN: return "FIRST_RUN";
    case DD_BOOT_NO_WIFI:   return "NO_WIFI";
    case DD_BOOT_NORMAL:    return "NORMAL";
    default:                return "?";
    }
}

esp_err_t dd_config_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: erasing chassis NVS namespaces");
    // Wipe every chassis namespace so a forgotten-password reset also
    // drops LED preference, NTP server choice, BLE presence_timeout,
    // etc. App-side state (events.jsonl, worker registry) is wiped
    // separately by the caller (factory_reset_post in dd_http_system).
    static const char *NAMESPACES[] = {
        NS, "led", "time", "presence",
    };
    esp_err_t last_err = ESP_OK;
    for (size_t i = 0; i < sizeof(NAMESPACES) / sizeof(NAMESPACES[0]); i++) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(NAMESPACES[i], NVS_READWRITE, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND) continue;
        if (err != ESP_OK) { last_err = err; continue; }
        esp_err_t e = nvs_erase_all(h);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
        if (e != ESP_OK) last_err = e;
    }
    return last_err;
}

esp_err_t dd_config_get_device_name(char *out, size_t cap)
{
    if (!out || cap == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t sz = cap;
    esp_err_t err = nvs_get_str(h, KEY_DEVICE_NAME, out, &sz);
    nvs_close(h);
    return err;
}

esp_err_t dd_config_set_device_name(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) >= DD_DEVICE_NAME_MAX)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_DEVICE_NAME, name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "device_name set to %s", name);
    return err;
}

#define KEY_BOOT_COUNT     "boot_count"
#define KEY_TOTAL_UPTIME   "total_up_s"
#define KEY_LAST_UPTIME    "last_up_s"
#define KEY_RESTART_CAUSE  "rst_cause"
#define KEY_BOOT_LOOP      "boot_loop"


// Cached at boot from NVS so multiple consumers (status, diag) see a stable
// value for this boot's "what triggered the previous shutdown" answer.
static dd_restart_cause_t s_last_cause       = DD_RESTART_UNKNOWN;
static bool               s_last_cause_init  = false;

esp_err_t dd_metrics_load(dd_metrics_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    nvs_get_u32(h, KEY_BOOT_COUNT,   &out->boot_count);
    nvs_get_u64(h, KEY_TOTAL_UPTIME, &out->total_uptime_s);
    nvs_get_u32(h, KEY_LAST_UPTIME,  &out->last_uptime_s);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t dd_metrics_record_boot(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint32_t n = 0;
    nvs_get_u32(h, KEY_BOOT_COUNT, &n);
    n++;
    err = nvs_set_u32(h, KEY_BOOT_COUNT, n);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "boot #%u", (unsigned)n);
    return err;
}

esp_err_t dd_metrics_save_uptime(uint32_t seconds)
{
    // Caller passes cumulative seconds-since-this-boot (esp_timer_get_time/1e6)
    // every minute. Only add the delta since last save — the previous version
    // added the whole cumulative each time, so total grew quadratically: a
    // ~2-hour boot contributed ~5 days to the running total. Static resets to
    // 0 every boot, so the first save adds the full ~60s of this boot, which
    // is correct.
    static uint32_t last_saved = 0;
    uint32_t delta = (seconds > last_saved) ? (seconds - last_saved) : seconds;
    last_saved = seconds;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint64_t total = 0;
    nvs_get_u64(h, KEY_TOTAL_UPTIME, &total);
    total += delta;
    err = nvs_set_u64(h, KEY_TOTAL_UPTIME, total);
    if (err == ESP_OK) err = nvs_set_u32(h, KEY_LAST_UPTIME, seconds);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t dd_metrics_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_BOOT_COUNT);
    nvs_erase_key(h, KEY_TOTAL_UPTIME);
    nvs_erase_key(h, KEY_LAST_UPTIME);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "metrics reset");
    return err;
}

// ---------- boot-loop recovery ----------

uint32_t dd_metrics_boot_loop_inc(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return 0;
    uint32_t n = 0;
    nvs_get_u32(h, KEY_BOOT_LOOP, &n);
    n++;
    nvs_set_u32(h, KEY_BOOT_LOOP, n);
    nvs_commit(h);
    nvs_close(h);
    return n;
}

uint32_t dd_metrics_boot_loop_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t n = 0;
    nvs_get_u32(h, KEY_BOOT_LOOP, &n);
    nvs_close(h);
    return n;
}

void dd_metrics_boot_loop_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t prev = 0;
    nvs_get_u32(h, KEY_BOOT_LOOP, &prev);
    if (prev != 0) {
        nvs_erase_key(h, KEY_BOOT_LOOP);
        nvs_commit(h);
        ESP_LOGI(TAG, "boot stable, cleared boot-loop counter (was %u)",
                 (unsigned)prev);
    }
    nvs_close(h);
}

bool dd_metrics_in_recovery_mode(void)
{
    return dd_metrics_boot_loop_count() >= DD_BOOT_LOOP_RECOVERY_THRESHOLD;
}

void dd_metrics_set_restart_cause(dd_restart_cause_t c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_RESTART_CAUSE, (uint8_t)c);
    nvs_commit(h);
    nvs_close(h);
}

void dd_metrics_consume_restart_cause(void)
{
    if (s_last_cause_init) return;
    s_last_cause_init = true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, KEY_RESTART_CAUSE, &v) == ESP_OK) {
        s_last_cause = (dd_restart_cause_t)v;
    }
    nvs_erase_key(h, KEY_RESTART_CAUSE);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "last restart cause: %s", dd_restart_cause_str(s_last_cause));
}

dd_restart_cause_t dd_metrics_get_last_cause(void)
{
    return s_last_cause;
}

const char *dd_restart_cause_str(dd_restart_cause_t c)
{
    switch (c) {
    case DD_RESTART_UNKNOWN:       return "unknown";
    case DD_RESTART_OTA:           return "ota";
    case DD_RESTART_ADMIN:         return "admin";
    case DD_RESTART_FACTORY:       return "factory";
    case DD_RESTART_SETUP:         return "setup";
    case DD_RESTART_HEAP_CRITICAL: return "heap_critical";
    case DD_RESTART_BOOT_LOOP:     return "boot_loop_recovery";
    default:                       return "?";
    }
}
