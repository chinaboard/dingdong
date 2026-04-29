// Internal header shared between dd_http.c and the dd_http_*.c sibling files.
// Not exported to other components — those still consume the public dd_http.h.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#define COOKIE_NAME "dd_session"

#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

// ---------- shared helpers (defined in dd_http.c) ----------

esp_err_t reply_json_status(httpd_req_t *req, const char *status, cJSON *body);
esp_err_t reply_text(httpd_req_t *req, const char *status, const char *msg);
cJSON    *recv_json_body(httpd_req_t *req);
bool      is_authed(httpd_req_t *req);
esp_err_t require_auth(httpd_req_t *req);
bool      extract_cookie_value(httpd_req_t *req, const char *name,
                                char *out, size_t cap);
void      schedule_restart(int delay_ms);

// Format a 6-byte BLE address into "xx:xx:xx:xx:xx:xx" (17 chars + NUL).
// Caller passes a buffer of at least 18 bytes.
void      format_mac(const uint8_t addr[6], char out[18]);

// ---------- handlers ----------
//
// Each handler is non-static so the route table in dd_http.c can reference it
// while its body lives in a sibling file.

// system / auth / OTA / backup
esp_err_t health_get(httpd_req_t *req);
esp_err_t metrics_get(httpd_req_t *req);
esp_err_t status_get(httpd_req_t *req);
esp_err_t diag_get(httpd_req_t *req);
esp_err_t restart_post(httpd_req_t *req);
esp_err_t factory_reset_post(httpd_req_t *req);
esp_err_t time_resync_post(httpd_req_t *req);
esp_err_t device_name_get(httpd_req_t *req);
esp_err_t device_name_post(httpd_req_t *req);
esp_err_t led_get(httpd_req_t *req);
esp_err_t led_post(httpd_req_t *req);
esp_err_t presence_timeout_get(httpd_req_t *req);
esp_err_t presence_timeout_post(httpd_req_t *req);
esp_err_t ntp_server_get(httpd_req_t *req);
esp_err_t ntp_server_post(httpd_req_t *req);
esp_err_t backup_get(httpd_req_t *req);
esp_err_t restore_post(httpd_req_t *req);
esp_err_t ota_post(httpd_req_t *req);
esp_err_t auth_login_post(httpd_req_t *req);
esp_err_t auth_logout_post(httpd_req_t *req);
esp_err_t auth_me_get(httpd_req_t *req);
esp_err_t auth_change_password_post(httpd_req_t *req);

// attendance (events / today / calendar)
esp_err_t events_get(httpd_req_t *req);
esp_err_t events_post(httpd_req_t *req);
esp_err_t events_wipe_post(httpd_req_t *req);
esp_err_t events_delete_post(httpd_req_t *req);
esp_err_t events_export_get(httpd_req_t *req);
esp_err_t events_paired_get(httpd_req_t *req);
esp_err_t today_get(httpd_req_t *req);
esp_err_t calendar_get(httpd_req_t *req);

// device management (workers / bonds / pairing / setup / wifi)
esp_err_t workers_get(httpd_req_t *req);
esp_err_t workers_update_post(httpd_req_t *req);
esp_err_t workers_delete_post(httpd_req_t *req);
esp_err_t bonds_get(httpd_req_t *req);
esp_err_t bonds_revoke_post(httpd_req_t *req);
esp_err_t pairing_start_post(httpd_req_t *req);
esp_err_t pairing_cancel_post(httpd_req_t *req);
esp_err_t pairing_status_get(httpd_req_t *req);
esp_err_t pairing_confirm_post(httpd_req_t *req);
esp_err_t setup_post(httpd_req_t *req);
esp_err_t wifi_scan_get(httpd_req_t *req);
