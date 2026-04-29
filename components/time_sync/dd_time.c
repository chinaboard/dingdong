#include "dd_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

// Mutex protects the 64-bit anchor pair + the NTP server buffer. Anchor pair
// is written from the SNTP `on_sync` callback (FreeRTOS task context) and
// read from HTTP handlers — without the lock, 64-bit reads on the 32-bit
// RISC-V chip can tear. NTP server string is similarly mutated by HTTP POST
// while another HTTP GET reads it.
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_synced = false;
static int64_t       s_wall_at_sync_us = 0;
static int64_t       s_mono_at_sync_us = 0;
static char          s_ntp_server[DD_NTP_SERVER_MAX] = NTP_SERVER_DEFAULT;
static bool          s_sntp_inited = false;

#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

static void on_sync(struct timeval *tv)
{
    int64_t wall = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
    int64_t mono = esp_timer_get_time();
    LOCK();
    s_synced = true;
    s_wall_at_sync_us = wall;
    s_mono_at_sync_us = mono;
    UNLOCK();
    ESP_LOGI(TAG, "NTP sync OK: unix=%lld", (long long)tv->tv_sec);
}

static void load_ntp_server(void)
{
    nvs_handle_t h;
    if (nvs_open(NTP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char buf[DD_NTP_SERVER_MAX];
    size_t sz = sizeof(buf);
    if (nvs_get_str(h, NTP_NVS_KEY, buf, &sz) == ESP_OK && buf[0]) {
        buf[sizeof(buf) - 1] = '\0';
        LOCK();
        strncpy(s_ntp_server, buf, sizeof(s_ntp_server) - 1);
        s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';
        UNLOCK();
    }
    nvs_close(h);
}

esp_err_t dd_time_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
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
    char server[DD_NTP_SERVER_MAX];
    LOCK();
    strncpy(server, s_ntp_server, sizeof(server) - 1);
    server[sizeof(server) - 1] = '\0';
    UNLOCK();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.start = true;
    cfg.sync_cb = on_sync;
    cfg.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_sntp_inited = true;
        ESP_LOGI(TAG, "SNTP started, server=%s", server);
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
    LOCK();
    int64_t delta_us = mono_us - s_mono_at_sync_us;
    int64_t wall_us  = s_wall_at_sync_us + delta_us;
    UNLOCK();
    return wall_us / 1000000;
}

const char *dd_time_get_ntp_server(void)
{
    // Returns pointer into shared buffer — caller copies if it needs stability
    // beyond the immediate use. HTTP handlers serialize replies anyway.
    return s_ntp_server;
}

esp_err_t dd_time_set_ntp_server(const char *server)
{
    if (!server || !server[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(server) >= DD_NTP_SERVER_MAX) return ESP_ERR_INVALID_SIZE;

    LOCK();
    strncpy(s_ntp_server, server, sizeof(s_ntp_server) - 1);
    s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';
    UNLOCK();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NTP_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, NTP_NVS_KEY, server);
        err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "ntp server -> %s", server);

    // Reset s_synced so the badge flips to "syncing" until the new server
    // answers; resync immediately.
    s_synced = false;
    dd_time_sntp_start();
    return err;
}

int64_t dd_time_last_sync_unix(void)
{
    LOCK();
    int64_t v = s_wall_at_sync_us;
    UNLOCK();
    return v / 1000000;
}
