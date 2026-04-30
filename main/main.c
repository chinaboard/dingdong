#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dd_config.h"
#include "dd_wifi.h"
#include "dd_http.h"
#include "dd_session.h"
#include "dd_ble.h"
#include "dd_time.h"
#include "dd_storage.h"
#include "dd_worker.h"
#include "dd_event.h"
#include "dd_led.h"
#include "dd_log.h"

static const char *TAG = "dingdong";

// After 60s of healthy uptime, mark current OTA image as valid so bootloader
// won't roll back next boot. If we panic before this fires, the bootloader
// reverts to the previous image automatically.
static void ota_mark_valid_cb(void *arg)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(p, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "OTA: marked image '%s' as valid (was pending): %s",
                 p->label, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "OTA: image '%s' state=%d (no action)", p->label, (int)state);
    }
}

static void schedule_ota_validation(void)
{
    static esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t args = {
        .callback = &ota_mark_valid_cb,
        .name = "ota_validate",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 60ULL * 1000 * 1000);  // 60s
    }
}

// Periodic heap watchdog. Logs warnings and restarts if heap is critically low.
#define HEAP_WARN_LIMIT     20480   // 20 KB
#define HEAP_CRITICAL_LIMIT 6144    // 6 KB
static int s_heap_warn_count = 0;

static void heap_check_cb(void *arg)
{
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < HEAP_CRITICAL_LIMIT) {
        ESP_LOGE(TAG, "HEAP CRITICAL: %u bytes free, restarting!", (unsigned)free_heap);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    if (free_heap < HEAP_WARN_LIMIT && s_heap_warn_count++ % 6 == 0) {
        // Log every minute (we tick every 10s × 6 = 60s)
        ESP_LOGW(TAG, "heap low: %u bytes free (min ever %u)",
                 (unsigned)free_heap,
                 (unsigned)esp_get_minimum_free_heap_size());
    }
}

static void schedule_heap_watchdog(void)
{
    static esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t args = {
        .callback = &heap_check_cb,
        .name = "heap_check",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 10ULL * 1000 * 1000);  // every 10s
    }
}

// Periodic time-based event log rotation. Runs daily; trims events older than
// DD_EVENTS_RETENTION_DAYS so the file stays bounded by ~3 months of activity
// rather than 512 KB (which is just a safety cap).
static void retention_cb(void *arg)
{
    if (!dd_time_is_synced()) return;  // need real wall time
    int64_t cutoff = dd_time_now_unix() - (int64_t)DD_EVENTS_RETENTION_DAYS * 86400;
    if (cutoff <= 0) return;
    dd_storage_event_rotate_by_time(cutoff);
}

static void schedule_retention(void)
{
    static esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t args = {
        .callback = &retention_cb,
        .name = "retention",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        // Daily. esp_timer accepts up to ~292 years, so 86400s is fine.
        esp_timer_start_periodic(t, 86400ULL * 1000 * 1000);
        // Also fire once shortly after boot (NTP usually syncs in <30s).
        esp_timer_handle_t once = NULL;
        const esp_timer_create_args_t once_args = {
            .callback = &retention_cb,
            .name = "retention_once",
        };
        if (esp_timer_create(&once_args, &once) == ESP_OK) {
            esp_timer_start_once(once, 60ULL * 1000 * 1000);  // 60s after boot
        }
    }
}

// Physical recovery: long-press BOOT button (GPIO9 on most ESP32-C6 dev
// boards, active-low) for 5 seconds → factory reset + restart. Useful when
// admin password is forgotten or device is wedged.
#define BOOT_BUTTON_GPIO 9
#define LONG_PRESS_MS    5000

static void boot_button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int64_t press_start_us = 0;
    int     last_log_sec = 0;
    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        if (level == 0) {
            // pressed (active-low)
            if (press_start_us == 0) {
                press_start_us = esp_timer_get_time();
                last_log_sec = 0;
                ESP_LOGW(TAG, "BOOT button pressed (hold 5s for factory reset)");
            }
            int dur_ms = (int)((esp_timer_get_time() - press_start_us) / 1000);
            int dur_sec = dur_ms / 1000;
            if (dur_sec > last_log_sec) {
                last_log_sec = dur_sec;
                ESP_LOGW(TAG, "BOOT button held %ds/5s", dur_sec);
            }
            if (dur_ms >= LONG_PRESS_MS) {
                ESP_LOGE(TAG, "BOOT long-press → FACTORY RESET");
                dd_config_factory_reset();
                dd_storage_event_wipe();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            if (press_start_us != 0) {
                ESP_LOGI(TAG, "BOOT button released (no action)");
                press_start_us = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void dump_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    ESP_LOGI(TAG, " chip=%s rev=%d.%d cores=%d",
             info.model == CHIP_ESP32C6 ? "ESP32-C6" :
             info.model == CHIP_ESP32C3 ? "ESP32-C3" : "?",
             info.revision / 100, info.revision % 100, info.cores);
    ESP_LOGI(TAG, " BT MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void app_main(void)
{
    // Give USB Serial/JTAG host time to re-enumerate after a flash so we can
    // capture early boot logs from a non-TTY shell.
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Hook ESP_LOG into a 4 KB ring buffer first so the boot banner + every
    // subsequent log call lands in the buffer that /api/system/logs serves.
    dd_log_init();

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, " dingdong-fw v%s booting", esp_app_get_description()->version);
    dump_chip();

    ESP_ERROR_CHECK(dd_config_init());
    ESP_ERROR_CHECK(dd_session_init());
    dd_metrics_record_boot();
    ESP_ERROR_CHECK(dd_time_init());
    ESP_ERROR_CHECK(dd_storage_init());
    ESP_ERROR_CHECK(dd_event_init());
    ESP_ERROR_CHECK(dd_worker_init());

    // Quiet down chatty subsystems for serial monitor readability.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);
    esp_log_level_set("BLE_INIT", ESP_LOG_WARN);

    dd_boot_mode_t mode = dd_config_boot_mode();
    ESP_LOGI(TAG, " boot mode: %s (admin=%d wifi=%d)",
             dd_boot_mode_str(mode),
             dd_config_has_admin(), dd_config_has_wifi());
    ESP_LOGI(TAG, "==========================================");

    ESP_ERROR_CHECK(dd_ble_start());
    ESP_ERROR_CHECK(dd_led_init());
    ESP_ERROR_CHECK(dd_wifi_start());
    ESP_ERROR_CHECK(dd_http_start());
    // SNTP requires WiFi/lwip up. Always start; if STA isn't connected yet,
    // it'll keep retrying.
    dd_time_sntp_start();

    // Now that everything's wired, schedule OTA validation. If anything
    // panics in the next 60s the bootloader will revert to previous image.
    schedule_ota_validation();
    schedule_heap_watchdog();
    schedule_retention();
    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 5, NULL);

    char ip[16];
    int tick = 0;
    int last_logged_state = -1;
    while (1) {
        dd_wifi_get_ip(ip, sizeof(ip));

        // Pack key state into a hash so we can suppress identical alive logs.
        int state_hash = (dd_wifi_state() << 16)
                       | (dd_ble_is_advertising() << 8)
                       | (dd_time_is_synced() << 7)
                       | ((unsigned)dd_storage_event_count() & 0x3F);
        bool state_changed = (state_hash != last_logged_state);
        bool tick_anchor   = (tick % 6 == 0);  // log at least every 60s

        if (state_changed || tick_anchor) {
            ESP_LOGI(TAG, "alive tick=%d wifi=%s ip=%s ble=%s adv=%d ntp=%d events=%u heap=%uK",
                     tick, dd_wifi_state_str(dd_wifi_state()),
                     ip[0] ? ip : "-",
                     dd_ble_device_name(),
                     dd_ble_is_advertising(),
                     dd_time_is_synced(),
                     (unsigned)dd_storage_event_count(),
                     (unsigned)(esp_get_free_heap_size() / 1024));
            last_logged_state = state_hash;
        }
        tick++;
        // Every minute, persist current uptime so we don't lose more than 60s
        // of accounting if power dies or we panic.
        if (tick_anchor) {
            dd_metrics_save_uptime((uint32_t)(esp_timer_get_time() / 1000000));
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
