// HTTP handlers: system status / auth / device admin / OTA / backup-restore /
// diagnostics. Split out of dd_http.c to keep that file focused on the server
// lifecycle.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dd_config.h"
#include "dd_session.h"
#include "dd_ble.h"
#include "dd_led.h"
#include "dd_storage.h"
#include "dd_time.h"
#include "dd_wifi.h"
#include "dd_worker.h"

#include "http_internal.h"

static const char *TAG = "http.sys";

// ---------- public health & metrics ----------

esp_err_t health_get(httpd_req_t *req)
{
    bool ble_ok   = dd_ble_is_advertising();
    bool wifi_ok  = (dd_wifi_state() == DD_WIFI_STATE_AP ||
                     dd_wifi_state() == DD_WIFI_STATE_STA_GOT_IP);
    bool heap_ok  = esp_get_free_heap_size() > 20480;

    bool ok = ble_ok && wifi_ok && heap_ok;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_status(req, ok ? "200 OK" : "503 Service Unavailable");

    char body[160];
    int n = snprintf(body, sizeof(body),
                     "%s ble=%d wifi=%d heap_ok=%d uptime_s=%lld\n",
                     ok ? "OK" : "DEGRADED",
                     ble_ok, wifi_ok, heap_ok,
                     (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

esp_err_t metrics_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    dd_metrics_t m = { 0 };
    dd_metrics_load(&m);

    uint8_t addrs[8][6];
    int bonds_n = dd_ble_bond_list(addrs, 8);

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "uptime_seconds %lld\n"
        "boot_count %u\n"
        "total_uptime_seconds %llu\n"
        "last_uptime_seconds %u\n"
        "heap_free_bytes %u\n"
        "heap_min_free_bytes %u\n"
        "events_count %u\n"
        "events_file_bytes %u\n"
        "workers_count %u\n"
        "bonds_count %d\n"
        "ble_advertising %d\n"
        "ble_pairing_active %d\n"
        "wifi_state %d\n"
        "ntp_synced %d\n",
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned)m.boot_count,
        (unsigned long long)m.total_uptime_s,
        (unsigned)m.last_uptime_s,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)dd_storage_event_count(),
        (unsigned)dd_storage_event_bytes(),
        (unsigned)dd_worker_count(),
        bonds_n,
        dd_ble_is_advertising(),
        dd_ble_pairing_active(),
        (int)dd_wifi_state(),
        dd_time_is_synced());

    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

esp_err_t status_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", dd_boot_mode_str(dd_config_boot_mode()));
    cJSON_AddStringToObject(j, "wifi", dd_wifi_state_str(dd_wifi_state()));
    char ip[16]; dd_wifi_get_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddBoolToObject(j, "has_admin", dd_config_has_admin());
    cJSON_AddBoolToObject(j, "has_wifi_creds", dd_config_has_wifi());
    cJSON_AddStringToObject(j, "ble", dd_ble_device_name());
    cJSON_AddBoolToObject(j, "ble_advertising", dd_ble_is_advertising());
    cJSON_AddBoolToObject(j, "pairing_active", dd_ble_pairing_active());
    cJSON_AddBoolToObject(j, "ntp_synced", dd_time_is_synced());
    cJSON_AddNumberToObject(j, "now_unix", (double)dd_time_now_unix());
    cJSON_AddNumberToObject(j, "events_count", (double)dd_storage_event_count());
    cJSON_AddNumberToObject(j, "events_bytes", (double)dd_storage_event_bytes());
    cJSON_AddNumberToObject(j, "workers_count", (double)dd_worker_count());
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(j, "heap_free", (double)esp_get_free_heap_size());

    // mDNS hostname so the UI (and the setup-saved screen) can show users
    // a stable URL for after-reboot access. Always BT-MAC-derived; doesn't
    // track the user-changeable BLE display name.
    {
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
        char host[24];
        snprintf(host, sizeof(host), "dingdong-%02x%02x", mac[4], mac[5]);
        cJSON_AddStringToObject(j, "mdns_host", host);
    }

    // Firmware version on the public status — useful in the header pre-login.
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) cJSON_AddStringToObject(j, "version", desc->version);

    // LittleFS usage so the Web UI can render a "storage" widget without
    // hitting /api/system/diag every poll.
    size_t fs_total = 0, fs_used = 0;
    if (dd_storage_fs_info(&fs_total, &fs_used) == ESP_OK) {
        cJSON_AddNumberToObject(j, "fs_total", (double)fs_total);
        cJSON_AddNumberToObject(j, "fs_used",  (double)fs_used);
    }
    return reply_json_status(req, "200 OK", j);
}

// ---------- auth ----------

esp_err_t auth_login_post(httpd_req_t *req)
{
    if (!dd_config_has_admin()) {
        return reply_text(req, "409 Conflict", "no admin set; use /api/setup first");
    }
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *pw = cJSON_GetObjectItem(j, "password");
    if (!cJSON_IsString(pw)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "password required");
    }

    bool ok = false;
    esp_err_t err = dd_config_verify_admin_password(pw->valuestring, &ok);
    cJSON_Delete(j);

    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "verify failed");
    if (!ok) {
        // crude rate-limit: sleep on miss
        vTaskDelay(pdMS_TO_TICKS(800));
        return reply_text(req, "401 Unauthorized", "bad password");
    }

    char tok[DD_SESSION_TOKEN_LEN];
    if (dd_session_create(tok, sizeof(tok)) != ESP_OK) {
        return reply_text(req, "500 Internal Server Error", "session alloc failed");
    }

    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=604800",
             tok);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return reply_text(req, "200 OK", "ok");
}

esp_err_t auth_logout_post(httpd_req_t *req)
{
    char tok[DD_SESSION_TOKEN_LEN];
    if (extract_cookie_value(req, COOKIE_NAME, tok, sizeof(tok))) {
        dd_session_destroy(tok);
    }
    httpd_resp_set_hdr(req, "Set-Cookie",
                       COOKIE_NAME "=; Path=/; HttpOnly; Max-Age=0");
    return reply_text(req, "200 OK", "ok");
}

esp_err_t auth_me_get(httpd_req_t *req)
{
    if (is_authed(req)) return reply_text(req, "200 OK", "authed");
    return reply_text(req, "401 Unauthorized", "not authed");
}

esp_err_t auth_change_password_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *old_j = cJSON_GetObjectItem(j, "old_password");
    const cJSON *new_j = cJSON_GetObjectItem(j, "new_password");
    if (!cJSON_IsString(old_j) || !cJSON_IsString(new_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "old_password + new_password required");
    }
    if (strlen(new_j->valuestring) < 8) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "new password too short");
    }

    bool ok = false;
    if (dd_config_verify_admin_password(old_j->valuestring, &ok) != ESP_OK || !ok) {
        cJSON_Delete(j);
        vTaskDelay(pdMS_TO_TICKS(800));
        return reply_text(req, "401 Unauthorized", "old password wrong");
    }

    esp_err_t err = dd_config_set_admin_password(new_j->valuestring);
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "set failed");

    dd_session_destroy_all();
    return reply_text(req, "200 OK", "password changed; all sessions revoked");
}

// ---------- system control ----------

esp_err_t restart_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    reply_text(req, "200 OK", "restarting in 1s");
    schedule_restart(1000);
    return ESP_OK;
}

esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *conf_j = cJSON_GetObjectItem(j, "confirm");
    bool confirmed = cJSON_IsString(conf_j) &&
                     strcmp(conf_j->valuestring, "FACTORY_RESET") == 0;
    cJSON_Delete(j);
    if (!confirmed) {
        return reply_text(req, "400 Bad Request",
                          "must POST {\"confirm\":\"FACTORY_RESET\"}");
    }

    ESP_LOGW(TAG, "factory reset requested via web");

    dd_config_factory_reset();
    dd_storage_event_wipe();

    reply_text(req, "200 OK", "factory reset, restarting");
    schedule_restart(1500);
    return ESP_OK;
}

esp_err_t time_resync_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    esp_err_t err = dd_time_sntp_start();
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "sntp restart failed");
    return reply_text(req, "200 OK", "ntp resync triggered");
}

esp_err_t device_name_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", dd_ble_device_name());
    return reply_json_status(req, "200 OK", r);
}

esp_err_t device_name_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *n = cJSON_GetObjectItem(j, "name");
    if (!cJSON_IsString(n) || strlen(n->valuestring) == 0 ||
        strlen(n->valuestring) >= DD_DEVICE_NAME_MAX) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request",
                          "name required (1.." STRINGIFY(DD_DEVICE_NAME_MAX) " chars)");
    }
    esp_err_t err = dd_config_set_device_name(n->valuestring);
    if (err == ESP_OK) dd_ble_set_device_name(n->valuestring);
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");
    return reply_text(req, "200 OK",
        "ok (already-bonded iPhones still show old name until re-paired)");
}

// ---------- LED runtime toggle ----------

esp_err_t led_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", dd_led_is_enabled());
    return reply_json_status(req, "200 OK", r);
}

esp_err_t led_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *en = cJSON_GetObjectItem(j, "enabled");
    if (!cJSON_IsBool(en)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "enabled (bool) required");
    }
    esp_err_t err = dd_led_set_enabled(cJSON_IsTrue(en));
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");
    return reply_text(req, "200 OK", "ok");
}

// ---------- presence timeout ----------

esp_err_t presence_timeout_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "timeout_s", dd_ble_get_presence_timeout_s());
    return reply_json_status(req, "200 OK", r);
}

esp_err_t presence_timeout_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *t = cJSON_GetObjectItem(j, "timeout_s");
    if (!cJSON_IsNumber(t)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "timeout_s (int) required");
    }
    esp_err_t err = dd_ble_set_presence_timeout_s((int)t->valuedouble);
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");
    return reply_text(req, "200 OK", "ok");
}

// ---------- NTP server ----------

esp_err_t ntp_server_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "server", dd_time_get_ntp_server());
    cJSON_AddBoolToObject  (r, "synced", dd_time_is_synced());
    return reply_json_status(req, "200 OK", r);
}

esp_err_t ntp_server_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *s = cJSON_GetObjectItem(j, "server");
    if (!cJSON_IsString(s) || !s->valuestring[0]) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "server (string) required");
    }
    esp_err_t err = dd_time_set_ntp_server(s->valuestring);
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_SIZE) return reply_text(req, "400 Bad Request", "server too long");
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");
    return reply_text(req, "200 OK", "ok, resyncing");
}

// ---------- backup / restore ----------
//
// Backup contains everything needed to recreate the device's logical state on
// a fresh install: wifi creds, admin pw hash+salt, workers list. BLE bond
// keys (LTK/IRK) are NOT included — they're per-device secrets that require
// re-pairing on the new hardware.
//
// Restore is permitted ONLY when no admin is set (i.e. fresh install or after
// factory reset), so it can't be abused to overwrite an active config.

static char hex_nibble(uint8_t n)
{
    return n < 10 ? '0' + n : 'a' + (n - 10);
}

static void hex_encode_str(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex_nibble(in[i] >> 4);
        out[i * 2 + 1] = hex_nibble(in[i] & 0xF);
    }
    out[n * 2] = '\0';
}

static int hex_decode(const char *in, uint8_t *out, size_t out_cap)
{
    size_t in_len = strlen(in);
    if (in_len % 2 != 0 || in_len / 2 > out_cap) return -1;
    for (size_t i = 0; i < in_len / 2; i++) {
        char hi = in[i*2], lo = in[i*2+1];
        int v = 0;
        if      (hi >= '0' && hi <= '9') v |= (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') v |= (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') v |= (hi - 'A' + 10) << 4;
        else return -1;
        if      (lo >= '0' && lo <= '9') v |= (lo - '0');
        else if (lo >= 'a' && lo <= 'f') v |= (lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') v |= (lo - 'A' + 10);
        else return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(in_len / 2);
}

static esp_err_t worker_to_backup_cb(const dd_worker_t *w, void *arg)
{
    cJSON *arr = arg;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "id", w->id);
    char addr_str[18];
    format_mac(w->addr, addr_str);
    cJSON_AddStringToObject(e, "addr", addr_str);
    cJSON_AddStringToObject(e, "name", w->name);
    cJSON_AddStringToObject(e, "category", w->category);
    cJSON_AddNumberToObject(e, "created_at", (double)w->created_at);
    cJSON_AddItemToArray(arr, e);
    return ESP_OK;
}

esp_err_t backup_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "created_at", (double)time(NULL));

    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device_mac", mac_str);

    cJSON *cfg = cJSON_CreateObject();
    char ssid[DD_WIFI_SSID_MAX] = {0};
    char pass[DD_WIFI_PASS_MAX] = {0};
    if (dd_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        cJSON_AddStringToObject(cfg, "wifi_ssid", ssid);
        cJSON_AddStringToObject(cfg, "wifi_pass", pass);
    }

    uint8_t salt[DD_ADMIN_SALT_LEN], hash[DD_ADMIN_HASH_LEN];
    if (dd_config_get_admin_blob(salt, hash) == ESP_OK) {
        char hex[DD_ADMIN_HASH_LEN * 2 + 1];
        hex_encode_str(salt, DD_ADMIN_SALT_LEN, hex);
        cJSON_AddStringToObject(cfg, "admin_salt_hex", hex);
        hex_encode_str(hash, DD_ADMIN_HASH_LEN, hex);
        cJSON_AddStringToObject(cfg, "admin_hash_hex", hex);
    }
    cJSON_AddItemToObject(root, "config", cfg);

    cJSON *ws = cJSON_CreateArray();
    dd_worker_iter(worker_to_backup_cb, ws);
    cJSON_AddItemToObject(root, "workers", ws);

    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"dingdong-backup.json\"");
    return reply_json_status(req, "200 OK", root);
}

esp_err_t restore_post(httpd_req_t *req)
{
    if (dd_config_has_admin()) {
        return reply_text(req, "409 Conflict",
            "device already configured; factory_reset first");
    }

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *ver = cJSON_GetObjectItem(j, "version");
    if (!cJSON_IsNumber(ver) || (int)ver->valuedouble != 1) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "unsupported version");
    }

    cJSON *cfg = cJSON_GetObjectItem(j, "config");
    if (cJSON_IsObject(cfg)) {
        const cJSON *ssid_j = cJSON_GetObjectItem(cfg, "wifi_ssid");
        const cJSON *pass_j = cJSON_GetObjectItem(cfg, "wifi_pass");
        const cJSON *salt_j = cJSON_GetObjectItem(cfg, "admin_salt_hex");
        const cJSON *hash_j = cJSON_GetObjectItem(cfg, "admin_hash_hex");

        if (cJSON_IsString(ssid_j)) {
            dd_config_set_wifi(ssid_j->valuestring,
                cJSON_IsString(pass_j) ? pass_j->valuestring : "");
        }

        if (cJSON_IsString(salt_j) && cJSON_IsString(hash_j)) {
            uint8_t salt[DD_ADMIN_SALT_LEN], hash[DD_ADMIN_HASH_LEN];
            if (hex_decode(salt_j->valuestring, salt, sizeof(salt)) == DD_ADMIN_SALT_LEN &&
                hex_decode(hash_j->valuestring, hash, sizeof(hash)) == DD_ADMIN_HASH_LEN) {
                dd_config_set_admin_blob(salt, hash);
            }
        }
    }

    cJSON *ws = cJSON_GetObjectItem(j, "workers");
    if (cJSON_IsArray(ws)) {
        dd_worker_wipe();
        cJSON *e;
        cJSON_ArrayForEach(e, ws) {
            dd_worker_t w = { 0 };
            const cJSON *id_j   = cJSON_GetObjectItem(e, "id");
            const cJSON *addr_j = cJSON_GetObjectItem(e, "addr");
            const cJSON *name_j = cJSON_GetObjectItem(e, "name");
            const cJSON *cat_j  = cJSON_GetObjectItem(e, "category");
            const cJSON *ct_j   = cJSON_GetObjectItem(e, "created_at");
            if (!cJSON_IsNumber(id_j)) continue;
            w.id = (uint16_t)id_j->valuedouble;
            if (cJSON_IsString(addr_j)) {
                unsigned int a[6];
                if (sscanf(addr_j->valuestring,
                           "%02x:%02x:%02x:%02x:%02x:%02x",
                           &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
                    for (int i = 0; i < 6; i++) w.addr[i] = (uint8_t)a[i];
                }
            }
            if (cJSON_IsString(name_j))
                strncpy(w.name, name_j->valuestring, sizeof(w.name) - 1);
            if (cJSON_IsString(cat_j))
                strncpy(w.category, cat_j->valuestring, sizeof(w.category) - 1);
            if (cJSON_IsNumber(ct_j))
                w.created_at = (int64_t)ct_j->valuedouble;
            dd_worker_restore(&w);
        }
    }

    cJSON_Delete(j);
    reply_text(req, "200 OK", "restored, restarting");
    schedule_restart(1500);
    return ESP_OK;
}

// ---------- diag ----------

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
    }
}

esp_err_t diag_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = cJSON_CreateObject();

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        cJSON *fw = cJSON_CreateObject();
        cJSON_AddStringToObject(fw, "project_name", desc->project_name);
        cJSON_AddStringToObject(fw, "version", desc->version);
        cJSON_AddStringToObject(fw, "idf_ver", desc->idf_ver);
        cJSON_AddStringToObject(fw, "compile_date", desc->date);
        cJSON_AddStringToObject(fw, "compile_time", desc->time);
        cJSON_AddItemToObject(j, "firmware", fw);
    }

    esp_chip_info_t info; esp_chip_info(&info);
    cJSON *chip = cJSON_CreateObject();
    cJSON_AddStringToObject(chip, "model",
        info.model == CHIP_ESP32C6 ? "ESP32-C6" : "?");
    cJSON_AddNumberToObject(chip, "rev_major", info.revision / 100);
    cJSON_AddNumberToObject(chip, "rev_minor", info.revision % 100);
    cJSON_AddNumberToObject(chip, "cores", info.cores);
    {
        uint8_t mac[6];
        char buf[18];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(chip, "mac_wifi", buf);
        esp_read_mac(mac, ESP_MAC_BT);
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(chip, "mac_bt", buf);
        char host[24];
        snprintf(host, sizeof(host), "dingdong-%02x%02x", mac[4], mac[5]);
        cJSON_AddStringToObject(chip, "mdns", host);
    }
    cJSON_AddItemToObject(j, "chip", chip);

    cJSON *heap = cJSON_CreateObject();
    cJSON_AddNumberToObject(heap, "free",        (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(heap, "min_free",    (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(heap, "largest_free",
        (double)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    cJSON_AddItemToObject(j, "heap", heap);

    size_t fs_total = 0, fs_used = 0;
    if (dd_storage_fs_info(&fs_total, &fs_used) == ESP_OK) {
        cJSON *fs = cJSON_CreateObject();
        cJSON_AddNumberToObject(fs, "total", (double)fs_total);
        cJSON_AddNumberToObject(fs, "used",  (double)fs_used);
        cJSON_AddNumberToObject(fs, "events_file_bytes",
                                (double)dd_storage_event_bytes());
        cJSON_AddItemToObject(j, "fs", fs);
    }

    cJSON_AddStringToObject(j, "reset_reason", reset_reason_str(esp_reset_reason()));

    dd_metrics_t m;
    if (dd_metrics_load(&m) == ESP_OK) {
        cJSON *mj = cJSON_CreateObject();
        cJSON_AddNumberToObject(mj, "boot_count",     (double)m.boot_count);
        cJSON_AddNumberToObject(mj, "total_uptime_s", (double)m.total_uptime_s);
        cJSON_AddNumberToObject(mj, "last_uptime_s",  (double)m.last_uptime_s);
        cJSON_AddItemToObject(j, "metrics", mj);
    }

    cJSON *ret = cJSON_CreateObject();
    cJSON_AddNumberToObject(ret, "retention_days", DD_EVENTS_RETENTION_DAYS);
    cJSON_AddNumberToObject(ret, "max_bytes_cap",  DD_EVENTS_MAX_BYTES);
    cJSON_AddItemToObject(j, "events_retention", ret);

    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);

    cJSON_AddNumberToObject(j, "workers_count",  (double)dd_worker_count());
    cJSON_AddNumberToObject(j, "events_count",   (double)dd_storage_event_count());

    // Per-slot view: walk both ota_0/ota_1, read each app_desc and report
    // version + which one is running / which one OTA would write to next.
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    cJSON *ota = cJSON_CreateObject();
    cJSON *slots = cJSON_CreateArray();
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
            p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "label", p->label);
            cJSON_AddNumberToObject(s, "size", (double)p->size);
            esp_app_desc_t pdesc;
            if (esp_ota_get_partition_description(p, &pdesc) == ESP_OK) {
                cJSON_AddStringToObject(s, "version", pdesc.version);
            } else {
                cJSON_AddStringToObject(s, "version", "");
            }
            esp_ota_img_states_t st;
            if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
                const char *st_str =
                    st == ESP_OTA_IMG_NEW            ? "new" :
                    st == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify" :
                    st == ESP_OTA_IMG_VALID          ? "valid" :
                    st == ESP_OTA_IMG_INVALID        ? "invalid" :
                    st == ESP_OTA_IMG_ABORTED        ? "aborted" :
                    st == ESP_OTA_IMG_UNDEFINED      ? "undefined" : "?";
                cJSON_AddStringToObject(s, "state", st_str);
            }
            cJSON_AddBoolToObject(s, "running", running && p == running);
            cJSON_AddBoolToObject(s, "next",    next    && p == next);
            cJSON_AddItemToArray(slots, s);
        }
        it = esp_partition_next(it);
    }
    if (it) esp_partition_iterator_release(it);
    cJSON_AddItemToObject(ota, "slots", slots);
    if (running) cJSON_AddStringToObject(ota, "running", running->label);
    if (next)    cJSON_AddStringToObject(ota, "next",    next->label);
    cJSON_AddItemToObject(j, "ota", ota);

    return reply_json_status(req, "200 OK", j);
}

// ---------- OTA upload ----------
//
// POST raw .bin body to /api/system/ota. Streams to next OTA partition via
// esp_ota_*. On success, sets boot partition and reboots into the new image.

esp_err_t ota_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    int total = req->content_len;
    if (total <= 0) return reply_text(req, "400 Bad Request", "no content");

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) return reply_text(req, "500 Internal Server Error", "no ota slot");

    if ((uint32_t)total > update->size) {
        char msg[80];
        snprintf(msg, sizeof(msg), "image %d > slot %u",
                 total, (unsigned)update->size);
        return reply_text(req, "413 Payload Too Large", msg);
    }

    ESP_LOGW(TAG, "OTA: receiving %d bytes into '%s' (slot %u)",
             total, update->label, (unsigned)update->size);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        return reply_text(req, "500 Internal Server Error", "ota_begin failed");
    }

    char buf[1024];
    int received = 0;
    int last_log = 0;
    while (received < total) {
        int want = total - received;
        if (want > (int)sizeof(buf)) want = sizeof(buf);
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "recv failed at %d/%d (n=%d)", received, total, n);
            return reply_text(req, "500 Internal Server Error", "recv aborted");
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
            return reply_text(req, "500 Internal Server Error", "write failed");
        }
        received += n;
        if (received - last_log > 65536) {
            ESP_LOGI(TAG, "OTA progress: %d/%d (%d%%)",
                     received, total, received * 100 / total);
            last_log = received;
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err));
        return reply_text(req, "400 Bad Request",
                          err == ESP_ERR_OTA_VALIDATE_FAILED
                              ? "image validation failed"
                              : "ota_end failed");
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot: %s", esp_err_to_name(err));
        return reply_text(req, "500 Internal Server Error", "set_boot failed");
    }

    ESP_LOGW(TAG, "OTA complete (%d bytes), rebooting in 2s", received);
    reply_text(req, "200 OK", "ok, rebooting into new image");
    schedule_restart(2000);
    return ESP_OK;
}
