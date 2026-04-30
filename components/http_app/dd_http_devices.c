// HTTP handlers: device management — workers, BLE bonds, pairing window,
// first-run setup, WiFi scan.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"

#include "dd_ble.h"
#include "dd_config.h"
#include "dd_storage.h"
#include "dd_worker.h"

#include "http_internal.h"

static const char *TAG = "http.dev";

// ---------- pairing window ----------

esp_err_t pairing_start_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    int64_t ttl_ms = 30000;
    cJSON *j = recv_json_body(req);
    if (j) {
        const cJSON *t = cJSON_GetObjectItem(j, "ttl_ms");
        if (cJSON_IsNumber(t)) ttl_ms = (int64_t)t->valuedouble;
        cJSON_Delete(j);
    }

    if (dd_ble_pairing_start(ttl_ms) != ESP_OK) {
        return reply_text(req, "500 Internal Server Error", "pairing start failed");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "ttl_ms", ttl_ms);
    cJSON_AddNumberToObject(r, "remaining_ms", dd_ble_pairing_remaining_ms());
    return reply_json_status(req, "200 OK", r);
}

esp_err_t pairing_cancel_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    dd_ble_pairing_cancel();
    return reply_text(req, "200 OK", "ok");
}

esp_err_t pairing_status_get(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    bool active = dd_ble_pairing_active();
    cJSON_AddBoolToObject(r, "active", active);
    cJSON_AddNumberToObject(r, "remaining_ms", dd_ble_pairing_remaining_ms());
    // Only authed users see the live numcmp value (mid-pairing 6-digit
    // verification number from SC SM exchange).
    if (active && is_authed(req)) {
        cJSON_AddNumberToObject(r, "numcmp", dd_ble_pairing_numcmp());
    }
    return reply_json_status(req, "200 OK", r);
}

esp_err_t pairing_confirm_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    bool accept = true;
    cJSON *j = recv_json_body(req);
    if (j) {
        const cJSON *a = cJSON_GetObjectItem(j, "accept");
        if (cJSON_IsBool(a)) accept = cJSON_IsTrue(a);
        cJSON_Delete(j);
    }
    esp_err_t err = dd_ble_pairing_confirm(accept);
    if (err == ESP_ERR_INVALID_STATE)
        return reply_text(req, "404 Not Found", "no pairing awaiting confirm");
    if (err != ESP_OK)
        return reply_text(req, "500 Internal Server Error", "inject failed");
    return reply_text(req, "200 OK", accept ? "accepted" : "rejected");
}

// ---------- workers ----------

static esp_err_t worker_to_json_cb(const dd_worker_t *w, void *arg)
{
    cJSON *arr = arg;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", w->id);
    char addr_str[18];
    format_mac(w->addr, addr_str);
    cJSON_AddStringToObject(j, "addr", addr_str);
    cJSON_AddStringToObject(j, "name", w->name);
    cJSON_AddStringToObject(j, "category", w->category);
    cJSON_AddNumberToObject(j, "created_at", (double)w->created_at);
    cJSON_AddBoolToObject  (j, "bonded", dd_ble_bond_exists(w->addr));
    cJSON_AddItemToArray(arr, j);
    return ESP_OK;
}

esp_err_t workers_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    dd_worker_iter(worker_to_json_cb, arr);
    cJSON_AddItemToObject(root, "workers", arr);
    return reply_json_status(req, "200 OK", root);
}

esp_err_t workers_update_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *id_j   = cJSON_GetObjectItem(j, "id");
    const cJSON *name_j = cJSON_GetObjectItem(j, "name");
    const cJSON *cat_j  = cJSON_GetObjectItem(j, "category");

    if (!cJSON_IsNumber(id_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "id required");
    }

    uint16_t id = (uint16_t)id_j->valuedouble;
    const char *name = cJSON_IsString(name_j) ? name_j->valuestring : NULL;
    const char *cat  = cJSON_IsString(cat_j)  ? cat_j->valuestring  : NULL;

    esp_err_t err = dd_worker_update(id, name, cat);
    cJSON_Delete(j);
    if (err == ESP_ERR_NOT_FOUND) return reply_text(req, "404 Not Found", "no such worker");
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "update failed");
    return reply_text(req, "200 OK", "ok");
}

esp_err_t workers_delete_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *id_j = cJSON_GetObjectItem(j, "id");
    if (!cJSON_IsNumber(id_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "id required");
    }
    uint16_t id = (uint16_t)id_j->valuedouble;
    cJSON_Delete(j);

    // Hard delete: drop BLE bond, drop all events for this worker, then drop
    // the worker slot itself. Single click → fully gone.
    dd_worker_t w;
    if (dd_worker_get(id, &w) == ESP_OK) {
        dd_ble_bond_revoke(w.addr);          // best-effort, ENOENT is fine
    }
    dd_storage_event_delete_by_worker(id);   // best-effort, no-op if no events
    esp_err_t err = dd_worker_delete(id);

    if (err == ESP_ERR_NOT_FOUND) return reply_text(req, "404 Not Found", "no such worker");
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "delete failed");
    return reply_text(req, "200 OK", "ok");
}

// ---------- bonds (low-level BLE bond store) ----------

esp_err_t bonds_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    uint8_t addrs[8][6];
    int n = dd_ble_bond_list(addrs, 8);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        char addr_str[18];
        format_mac(addrs[i], addr_str);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "addr", addr_str);
        cJSON_AddNumberToObject(e, "worker_id",
                                (double)dd_worker_find_by_addr(addrs[i]));
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(root, "bonds", arr);
    cJSON_AddNumberToObject(root, "count", n);
    cJSON_AddNumberToObject(root, "max",   8);   // BOND_MAX_LIST in dd_ble.c
    return reply_json_status(req, "200 OK", root);
}

esp_err_t bonds_revoke_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *addr_j = cJSON_GetObjectItem(j, "addr");
    if (!cJSON_IsString(addr_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "addr required");
    }

    uint8_t addr[6];
    unsigned int a[6];
    if (sscanf(addr_j->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "bad addr format");
    }
    for (int i = 0; i < 6; i++) addr[i] = (uint8_t)a[i];
    cJSON_Delete(j);

    esp_err_t err = dd_ble_bond_revoke(addr);
    if (err != ESP_OK) return reply_text(req, "404 Not Found", "no such bond");
    return reply_text(req, "200 OK", "ok");
}

// ---------- first-run setup + WiFi scan ----------

esp_err_t setup_post(httpd_req_t *req)
{
    if (dd_config_has_admin()) {
        return reply_text(req, "409 Conflict", "already configured");
    }
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *adm  = cJSON_GetObjectItem(j, "admin_password");
    const cJSON *ssid = cJSON_GetObjectItem(j, "wifi_ssid");
    const cJSON *pass = cJSON_GetObjectItem(j, "wifi_password");

    if (!cJSON_IsString(adm) || strlen(adm->valuestring) < 8) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "admin_password >= 8 chars");
    }

    esp_err_t err = dd_config_set_admin_password(adm->valuestring);
    if (err != ESP_OK) {
        cJSON_Delete(j);
        return reply_text(req, "500 Internal Server Error", "set admin failed");
    }

    if (cJSON_IsString(ssid) && strlen(ssid->valuestring) > 0) {
        const char *pwstr = cJSON_IsString(pass) ? pass->valuestring : "";
        err = dd_config_set_wifi(ssid->valuestring, pwstr);
        if (err != ESP_OK) {
            cJSON_Delete(j);
            return reply_text(req, "500 Internal Server Error", "set wifi failed");
        }
    }
    cJSON_Delete(j);

    reply_text(req, "200 OK", "ok, restarting in 2s");
    schedule_restart(2000);
    return ESP_OK;
}

esp_err_t wifi_scan_get(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        return reply_text(req, "503 Service Unavailable", "scan failed");
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;

    wifi_ap_record_t *recs = NULL;
    if (n > 0) {
        recs = calloc(n, sizeof(wifi_ap_record_t));
        if (!recs) return reply_text(req, "500 Internal Server Error", "oom");
        esp_wifi_scan_get_ap_records(&n, recs);
    }

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < n; ++i) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid",    (char *)recs[i].ssid);
        cJSON_AddNumberToObject(a, "rssi",    recs[i].rssi);
        cJSON_AddNumberToObject(a, "channel", recs[i].primary);
        cJSON_AddBoolToObject  (a, "secure",  recs[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, a);
    }
    free(recs);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "aps", arr);
    return reply_json_status(req, "200 OK", root);
}
