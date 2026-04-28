#include "dd_led.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#include "dd_wifi.h"
#include "dd_time.h"
#include "dd_ble.h"

// Build-time knobs (Makefile injects via -D…). Keep fallbacks so a bare
// `idf.py build` still works.
#ifndef DD_LED_ENABLE
#define DD_LED_ENABLE 1
#endif
#ifndef DD_LED_GPIO
#define DD_LED_GPIO 8
#endif
#ifndef DD_LED_BRIGHTNESS
#define DD_LED_BRIGHTNESS 10
#endif

#if DD_LED_ENABLE

#include "led_strip.h"

static const char *TAG = "led";

#define TICK_MS         100
#define HEAP_CRIT_BYTES 20480

typedef struct { uint8_t r, g, b; } rgb_t;

#define B DD_LED_BRIGHTNESS
static const rgb_t C_OFF    = {0, 0, 0};
static const rgb_t C_GREEN  = {0, B, 0};
static const rgb_t C_BLUE   = {0, 0, B};
static const rgb_t C_YELLOW = {B, B, 0};
static const rgb_t C_PURPLE = {B, 0, B};
static const rgb_t C_CYAN   = {0, B, B};
static const rgb_t C_RED    = {B, 0, 0};

static led_strip_handle_t s_strip = NULL;
static esp_timer_handle_t s_timer = NULL;
static rgb_t              s_last  = {0xff, 0xff, 0xff};  // force first push
static bool               s_runtime_enabled = true;       // loaded from NVS at init

#define NVS_NS         "led"
#define KEY_ENABLED    "enabled"

static rgb_t scale(rgb_t in, uint8_t num, uint8_t den)
{
    rgb_t out = {
        (uint8_t)((uint16_t)in.r * num / den),
        (uint8_t)((uint16_t)in.g * num / den),
        (uint8_t)((uint16_t)in.b * num / den),
    };
    return out;
}

static void push(rgb_t c)
{
    if (c.r == s_last.r && c.g == s_last.g && c.b == s_last.b) return;
    s_last = c;
    led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b);
    led_strip_refresh(s_strip);
}

static void tick_cb(void *arg)
{
    static uint32_t tick = 0;
    tick++;

    if (!s_runtime_enabled) {
        push(C_OFF);
        return;
    }

    // Animation phases
    bool     fast_on    = (tick / 2) & 1;          // toggle every 200 ms
    bool     slow_on    = (tick / 10) & 1;         // toggle every 1 s
    uint8_t  breath_pos = tick % 20;
    uint8_t  breath     = breath_pos < 10 ? breath_pos : (20 - breath_pos);  // 0..10

    rgb_t color = C_OFF;

    if (esp_get_free_heap_size() < HEAP_CRIT_BYTES) {
        color = fast_on ? C_RED : C_OFF;
    } else if (dd_ble_pairing_active()) {
        color = fast_on ? C_CYAN : C_OFF;
    } else {
        switch (dd_wifi_state()) {
        case DD_WIFI_STATE_AP:
            color = scale(C_PURPLE, breath, 10);
            break;
        case DD_WIFI_STATE_STA_CONNECTING:
        case DD_WIFI_STATE_DOWN:
            color = fast_on ? C_BLUE : C_OFF;
            break;
        case DD_WIFI_STATE_STA_GOT_IP:
            color = dd_time_is_synced()
                  ? C_GREEN
                  : (slow_on ? C_YELLOW : C_OFF);
            break;
        }
    }

    push(color);
}

esp_err_t dd_led_set_enabled(bool enabled)
{
    s_runtime_enabled = enabled;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_u8(h, KEY_ENABLED, enabled ? 1 : 0);
        err = nvs_commit(h);
        nvs_close(h);
    }
    if (!enabled && s_strip) {
        // Force off immediately so the user sees the toggle take effect
        // without waiting for the next tick.
        push(C_OFF);
    }
    ESP_LOGI(TAG, "runtime enable -> %s", enabled ? "on" : "off");
    return err;
}

bool dd_led_is_enabled(void) { return s_runtime_enabled; }

esp_err_t dd_led_init(void)
{
    // Load persisted on/off (default on if absent)
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 1;
            if (nvs_get_u8(h, KEY_ENABLED, &v) == ESP_OK) {
                s_runtime_enabled = v != 0;
            }
            nvs_close(h);
        }
    }

    led_strip_config_t scfg = {
        .strip_gpio_num   = DD_LED_GPIO,
        .max_leds         = 1,
        .led_model        = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = 0 },
    };
    led_strip_rmt_config_t rcfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };

    esp_err_t err = led_strip_new_rmt_device(&scfg, &rcfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip init failed (gpio=%d): %s — running without LED",
                 DD_LED_GPIO, esp_err_to_name(err));
        return ESP_OK;  // non-fatal
    }
    led_strip_clear(s_strip);

    const esp_timer_create_args_t args = {
        .callback = &tick_cb,
        .name     = "led_tick",
    };
    err = esp_timer_create(&args, &s_timer);
    if (err == ESP_OK) err = esp_timer_start_periodic(s_timer, TICK_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timer init failed: %s", esp_err_to_name(err));
        return ESP_OK;  // non-fatal
    }

    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d, brightness=%d, runtime=%s",
             DD_LED_GPIO, DD_LED_BRIGHTNESS, s_runtime_enabled ? "on" : "off");
    return ESP_OK;
}

#else   /* DD_LED_ENABLE == 0 */

esp_err_t dd_led_init(void) { return ESP_OK; }
esp_err_t dd_led_set_enabled(bool enabled) { (void)enabled; return ESP_OK; }
bool dd_led_is_enabled(void) { return false; }

#endif  /* DD_LED_ENABLE */
