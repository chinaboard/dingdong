// HTTP server lifecycle, route table, shared helpers, and the root handler
// that serves the embedded Web UI. Per-domain handlers live in:
//   dd_http_system.c       — auth, system, OTA, backup/restore, diag
//   dd_http_attendance.c   — events, today, calendar, CSV/JSONL export
//   dd_http_devices.c      — workers, bonds, pairing, setup, wifi scan

#include "dd_http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "dd_session.h"

#include "http_internal.h"

static const char *TAG = "http";

static httpd_handle_t s_server = NULL;

// Embedded Web UI (see web/index.html, gzipped + linked as a binary blob via
// the custom CMake step in CMakeLists.txt).
extern const uint8_t INDEX_HTML_GZ_START[] asm("_binary_index_html_gz_start");
extern const uint8_t INDEX_HTML_GZ_END[]   asm("_binary_index_html_gz_end");

// ---------- shared helpers ----------

static void delayed_restart_task(void *arg)
{
    int delay_ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGW(TAG, "restarting (delayed %d ms)", delay_ms);
    esp_restart();
}

void schedule_restart(int delay_ms)
{
    xTaskCreate(delayed_restart_task, "restart", 2048,
                (void *)(intptr_t)delay_ms, 5, NULL);
}

esp_err_t reply_json_status(httpd_req_t *req, const char *status, cJSON *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char *s = cJSON_PrintUnformatted(body);
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    cJSON_Delete(body);
    return r;
}

esp_err_t reply_text(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

cJSON *recv_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    // 8KB cap — large enough for backup JSON with 32 workers (~3KB) plus
    // some headroom; small enough not to blow heap on bogus uploads.
    if (total <= 0 || total > 8192) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[got] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

bool extract_cookie_value(httpd_req_t *req, const char *name,
                          char *out, size_t cap)
{
    size_t hlen = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hlen == 0 || hlen >= 1024) return false;
    char *buf = malloc(hlen + 1);
    if (!buf) return false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, hlen + 1) != ESP_OK) {
        free(buf); return false;
    }
    char prefix[64];
    int pn = snprintf(prefix, sizeof(prefix), "%s=", name);
    if (pn <= 0 || pn >= (int)sizeof(prefix)) { free(buf); return false; }

    bool ok = false;
    char *p = buf;
    while ((p = strstr(p, prefix)) != NULL) {
        if (p == buf || p[-1] == ' ' || p[-1] == ';') {
            p += pn;
            char *end = strpbrk(p, ";\r\n");
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            if (vlen < cap) {
                memcpy(out, p, vlen);
                out[vlen] = '\0';
                ok = true;
            }
            break;
        }
        p += pn;
    }
    free(buf);
    return ok;
}

bool is_authed(httpd_req_t *req)
{
    char tok[DD_SESSION_TOKEN_LEN];
    if (!extract_cookie_value(req, COOKIE_NAME, tok, sizeof(tok))) return false;
    return dd_session_is_valid(tok);
}

esp_err_t require_auth(httpd_req_t *req)
{
    if (is_authed(req)) return ESP_OK;
    reply_text(req, "401 Unauthorized", "auth required");
    return ESP_FAIL;
}

void format_mac(const uint8_t addr[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// ---------- root + favicon ----------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)INDEX_HTML_GZ_START,
                           INDEX_HTML_GZ_END - INDEX_HTML_GZ_START);
}

// 204 No Content so browsers stop asking. Cheaper than serving an actual icon.
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Captive-portal catch-all: any unknown path → 302 to "/". Combined with the
// captive_portal DNS hijack, this triggers iOS / Android / Windows network
// connectivity probes (captive.apple.com/hotspot-detect.html,
// connectivitycheck.gstatic.com/generate_204, etc.) to pop the OS's mini
// browser pointed at the setup page. Harmless in STA mode (any 404 there
// just becomes a redirect to a valid page).
static esp_err_t not_found_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------- start/stop ----------

esp_err_t dd_http_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.max_uri_handlers  = 48;
    cfg.stack_size        = 8192;
    cfg.recv_wait_timeout = 30;  // longer for OTA upload
    cfg.send_wait_timeout = 30;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",                         .method = HTTP_GET,  .handler = root_get          },
        { .uri = "/favicon.ico",              .method = HTTP_GET,  .handler = favicon_get       },
        { .uri = "/health",                   .method = HTTP_GET,  .handler = health_get        },
        { .uri = "/metrics",                  .method = HTTP_GET,  .handler = metrics_get       },
        { .uri = "/api/system/status",        .method = HTTP_GET,  .handler = status_get        },
        { .uri = "/api/wifi/scan",            .method = HTTP_GET,  .handler = wifi_scan_get     },
        { .uri = "/api/setup",                .method = HTTP_POST, .handler = setup_post        },
        { .uri = "/api/auth/login",           .method = HTTP_POST, .handler = auth_login_post   },
        { .uri = "/api/auth/logout",          .method = HTTP_POST, .handler = auth_logout_post  },
        { .uri = "/api/auth/me",              .method = HTTP_GET,  .handler = auth_me_get       },
        { .uri = "/api/auth/change_password", .method = HTTP_POST, .handler = auth_change_password_post },
        { .uri = "/api/system/restart",       .method = HTTP_POST, .handler = restart_post      },
        { .uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = factory_reset_post },
        { .uri = "/api/system/time_resync",   .method = HTTP_POST, .handler = time_resync_post  },
        { .uri = "/api/system/device_name",   .method = HTTP_GET,  .handler = device_name_get   },
        { .uri = "/api/system/device_name",   .method = HTTP_POST, .handler = device_name_post  },
        { .uri = "/api/system/led",           .method = HTTP_GET,  .handler = led_get           },
        { .uri = "/api/system/led",           .method = HTTP_POST, .handler = led_post          },
        { .uri = "/api/system/presence_timeout", .method = HTTP_GET,  .handler = presence_timeout_get },
        { .uri = "/api/system/presence_timeout", .method = HTTP_POST, .handler = presence_timeout_post },
        { .uri = "/api/system/backup",        .method = HTTP_GET,  .handler = backup_get        },
        { .uri = "/api/system/restore",       .method = HTTP_POST, .handler = restore_post      },
        { .uri = "/api/system/diag",          .method = HTTP_GET,  .handler = diag_get          },
        { .uri = "/api/system/ota",           .method = HTTP_POST, .handler = ota_post          },
        { .uri = "/api/pairing/start",        .method = HTTP_POST, .handler = pairing_start_post   },
        { .uri = "/api/pairing/cancel",       .method = HTTP_POST, .handler = pairing_cancel_post  },
        { .uri = "/api/pairing/status",       .method = HTTP_GET,  .handler = pairing_status_get   },
        { .uri = "/api/pairing/confirm",      .method = HTTP_POST, .handler = pairing_confirm_post },
        { .uri = "/api/events",               .method = HTTP_GET,  .handler = events_get           },
        { .uri = "/api/events",               .method = HTTP_POST, .handler = events_post          },
        { .uri = "/api/events/paired",        .method = HTTP_GET,  .handler = events_paired_get    },
        { .uri = "/api/events/export",        .method = HTTP_GET,  .handler = events_export_get    },
        { .uri = "/api/events/wipe",          .method = HTTP_POST, .handler = events_wipe_post     },
        { .uri = "/api/events/delete",        .method = HTTP_POST, .handler = events_delete_post   },
        { .uri = "/api/workers",              .method = HTTP_GET,  .handler = workers_get          },
        { .uri = "/api/workers/update",       .method = HTTP_POST, .handler = workers_update_post  },
        { .uri = "/api/workers/delete",       .method = HTTP_POST, .handler = workers_delete_post  },
        { .uri = "/api/bonds",                .method = HTTP_GET,  .handler = bonds_get            },
        { .uri = "/api/bonds/revoke",         .method = HTTP_POST, .handler = bonds_revoke_post    },
        { .uri = "/api/today",                .method = HTTP_GET,  .handler = today_get            },
        { .uri = "/api/calendar",             .method = HTTP_GET,  .handler = calendar_get         },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_redirect);

    ESP_LOGI(TAG, "HTTP server listening on :80 (%d routes)",
             (int)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}

esp_err_t dd_http_stop(void)
{
    if (!s_server) return ESP_OK;
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
