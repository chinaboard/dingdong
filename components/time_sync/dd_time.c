#include "dd_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "time";

#define NTP_SERVER "ntp.aliyun.com"

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

static void on_sync(struct timeval *tv)
{
    s_synced = true;
    s_wall_at_sync_us = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
    s_mono_at_sync_us = esp_timer_get_time();
    ESP_LOGI(TAG, "NTP sync OK: unix=%lld", (long long)tv->tv_sec);
}

esp_err_t dd_time_init(void)
{
    setenv("TZ", DD_TZ, 1);
    tzset();
    ESP_LOGI(TAG, "tz=%s, awaiting NTP", DD_TZ);
    return ESP_OK;
}

esp_err_t dd_time_sntp_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.start = true;
    cfg.sync_cb = on_sync;
    cfg.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        // already init'd — just continue
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SNTP started, server=%s", NTP_SERVER);
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
