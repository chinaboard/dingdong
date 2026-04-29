#include "dd_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "nvs.h"

static const char *TAG = "time";

#define NTP_SERVER_DEFAULT "ntp.aliyun.com"
#define NTP_NVS_NS         "time"
#define NTP_NVS_KEY        "ntp_server"

// POSIX TZ baked in at build time; used for device-side localtime needs (CSV
// export, calendar bucketing, today detection). Override via Makefile
// `-DDD_TZ='"JST-9"'`. The Web UI displays times in the browser's timezone, so
// this only matters for export/bucket boundaries.
#ifndef DD_TZ
#define DD_TZ "CST-8"
#endif

static volatile bool s_synced     = false;
static int64_t       s_wall_at_sync_us = 0;   // unix epoch micros at NTP sync moment
static int64_t       s_mono_at_sync_us = 0;
static char          s_ntp_server[DD_NTP_SERVER_MAX] = NTP_SERVER_DEFAULT;
static bool          s_sntp_inited = false;

static void on_sync(struct timeval *tv)
{
    s_synced = true;
    s_wall_at_sync_us = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
    s_mono_at_sync_us = esp_timer_get_time();
    ESP_LOGI(TAG, "NTP sync OK: unix=%lld", (long long)tv->tv_sec);
}

static void load_ntp_server(void)
{
    nvs_handle_t h;
    if (nvs_open(NTP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_ntp_server);
    char buf[DD_NTP_SERVER_MAX];
    if (nvs_get_str(h, NTP_NVS_KEY, buf, &sz) == ESP_OK && buf[0]) {
        strncpy(s_ntp_server, buf, sizeof(s_ntp_server) - 1);
        s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';
    }
    nvs_close(h);
}

esp_err_t dd_time_init(void)
{
    setenv("TZ", DD_TZ, 1);
    tzset();
    load_ntp_server();
    ESP_LOGI(TAG, "tz=%s, ntp=%s, awaiting sync", DD_TZ, s_ntp_server);
    return ESP_OK;
}

esp_err_t dd_time_sntp_start(void)
{
    // First call configures + starts. Subsequent calls (e.g. after server
    // change) deinit then re-init so the new server takes effect.
    if (s_sntp_inited) {
        esp_netif_sntp_deinit();
        s_sntp_inited = false;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    cfg.start = true;
    cfg.sync_cb = on_sync;
    cfg.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_sntp_inited = true;
        ESP_LOGI(TAG, "SNTP started, server=%s", s_ntp_server);
    } else {
        ESP_LOGE(TAG, "sntp_init=%s", esp_err_to_name(err));
    }
    return err;
}

bool dd_time_is_synced(void) { return s_synced; }

int64_t dd_time_now_unix(void)
{
    if (!s_synced) return 0;
    return (int64_t)time(NULL);
}

int64_t dd_time_mono_us(void)
{
    return esp_timer_get_time();
}

int64_t dd_time_mono_to_unix(int64_t mono_us)
{
    if (!s_synced) return 0;
    int64_t delta_us = mono_us - s_mono_at_sync_us;
    int64_t wall_us  = s_wall_at_sync_us + delta_us;
    return wall_us / 1000000;
}

const char *dd_time_get_ntp_server(void)
{
    return s_ntp_server;
}

esp_err_t dd_time_set_ntp_server(const char *server)
{
    if (!server || !server[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(server) >= DD_NTP_SERVER_MAX) return ESP_ERR_INVALID_SIZE;

    strncpy(s_ntp_server, server, sizeof(s_ntp_server) - 1);
    s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NTP_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, NTP_NVS_KEY, s_ntp_server);
        err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "ntp server -> %s", s_ntp_server);

    // Reset s_synced so the badge flips to "syncing" until the new server
    // answers; resync immediately.
    s_synced = false;
    dd_time_sntp_start();
    return err;
}
