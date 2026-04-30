// Status LED driver. Two backends behind one state machine:
//
//   DD_LED_KIND=1 (default)  WS2812 single addressable RGB pixel via espressif/led_strip
//   DD_LED_KIND=2            Plain GPIO LED via LEDC (PWM for dimming + breathe)
//
// The state machine is identical for both; only the rendering of a logical
// state into hardware differs. Plain LED has no colour, so colour-coded
// states collapse to brightness + blink rate as the only signals.

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
#ifndef DD_LED_KIND
#define DD_LED_KIND 1   // 1 = WS2812 RGB, 2 = plain GPIO LED via LEDC PWM
#endif

#if DD_LED_ENABLE

static const char *TAG = "led";

#define TICK_MS         100
#define HEAP_CRIT_BYTES 20480
#define NVS_NS          "led"
#define KEY_ENABLED     "enabled"

static esp_timer_handle_t s_timer = NULL;
static bool               s_runtime_enabled = true;       // loaded from NVS at init

// ---------- logical state shared by both backends ----------

typedef enum {
    LED_OFF,            // disabled or fully off
    LED_HEAP_CRIT,      // red fast blink
    LED_PAIRING,        // cyan fast blink
    LED_AP,             // purple breathe (SoftAP setup)
    LED_STA_DOWN,       // blue fast blink (connecting / down)
    LED_STA_NO_NTP,     // yellow slow blink (connected, no time yet)
    LED_OK,             // dim green steady
} led_state_t;

static led_state_t compute_state(void)
{
    if (esp_get_free_heap_size() < HEAP_CRIT_BYTES) return LED_HEAP_CRIT;
    if (dd_ble_pairing_active())                    return LED_PAIRING;
    switch (dd_wifi_state()) {
    case DD_WIFI_STATE_AP:             return LED_AP;
    case DD_WIFI_STATE_STA_CONNECTING:
    case DD_WIFI_STATE_DOWN:           return LED_STA_DOWN;
    case DD_WIFI_STATE_STA_GOT_IP:     return dd_time_is_synced() ? LED_OK : LED_STA_NO_NTP;
    }
    return LED_OFF;
}

// ============================================================================
// Backend 1: WS2812 RGB pixel
// ============================================================================
#if DD_LED_KIND == 1

#include "led_strip.h"

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
static rgb_t              s_last  = {0xff, 0xff, 0xff};  // force first push

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

static void render(led_state_t st, uint32_t tick)
{
    bool fast_on = (tick / 2)  & 1;             // toggle every 200 ms
    bool slow_on = (tick / 10) & 1;             // toggle every 1 s
    uint8_t bp   = tick % 20;
    uint8_t breath = bp < 10 ? bp : (20 - bp);  // 0..10

    switch (st) {
    case LED_OFF:        push(C_OFF); break;
    case LED_HEAP_CRIT:  push(fast_on ? C_RED : C_OFF); break;
    case LED_PAIRING:    push(fast_on ? C_CYAN : C_OFF); break;
    case LED_AP:         push(scale(C_PURPLE, breath, 10)); break;
    case LED_STA_DOWN:   push(fast_on ? C_BLUE : C_OFF); break;
    case LED_STA_NO_NTP: push(slow_on ? C_YELLOW : C_OFF); break;
    case LED_OK:         push(C_GREEN); break;
    }
}

static esp_err_t backend_init(void)
{
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
    if (err != ESP_OK) return err;
    led_strip_clear(s_strip);
    return ESP_OK;
}

static void backend_force_off(void) { push(C_OFF); }

static const char *backend_name(void) { return "WS2812 RGB"; }

// ============================================================================
// Backend 2: plain GPIO LED via LEDC PWM
// ============================================================================
#elif DD_LED_KIND == 2

#include "driver/ledc.h"

#define LEDC_TIMER     LEDC_TIMER_0
#define LEDC_MODE      LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL   LEDC_CHANNEL_0
#define LEDC_DUTY_RES  LEDC_TIMER_8_BIT          // 0-255 duty
#define LEDC_FREQ      5000                       // Hz, well above visible flicker

// Brightness presets (0-255 duty). Tuned so "OK" steady is gentle, blinks
// are eye-catching but not blinding even right next to the camera.
#define DUTY_OK        DD_LED_BRIGHTNESS         // ~10 by default = ~4% (gentle)
#define DUTY_BLINK     180                        // visible across the room
#define DUTY_FULL      255                        // alarm-grade

static uint8_t s_last_duty = 0xff;  // force first push

static void set_duty(uint8_t d)
{
    if (d == s_last_duty) return;
    s_last_duty = d;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, d);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void render(led_state_t st, uint32_t tick)
{
    bool fast_on = (tick / 2)  & 1;
    bool slow_on = (tick / 10) & 1;
    // 2 s breathe cycle: 0..255..0 over 20 ticks. Scale by DUTY_OK*5 ceiling
    // so the breathe never blasts brighter than ~50%.
    uint8_t bp = tick % 20;
    uint8_t bphase = bp < 10 ? bp : (20 - bp);     // 0..10
    uint8_t breathe_duty = (uint8_t)((bphase * 12));  // ~0..120

    switch (st) {
    case LED_OFF:        set_duty(0); break;
    case LED_HEAP_CRIT:  set_duty(fast_on ? DUTY_FULL  : 0); break;
    case LED_PAIRING:    set_duty(fast_on ? DUTY_BLINK : 0); break;
    case LED_AP:         set_duty(breathe_duty); break;
    case LED_STA_DOWN:   set_duty(fast_on ? DUTY_BLINK : 0); break;
    case LED_STA_NO_NTP: set_duty(slow_on ? DUTY_BLINK : 0); break;
    case LED_OK:         set_duty(DUTY_OK); break;
    }
}

static esp_err_t backend_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) return err;
    ledc_channel_config_t c = {
        .gpio_num   = DD_LED_GPIO,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    return ledc_channel_config(&c);
}

static void backend_force_off(void) { set_duty(0); }

static const char *backend_name(void) { return "GPIO LED (LEDC PWM)"; }

#else
#error "Unsupported DD_LED_KIND value (must be 1 for WS2812 or 2 for GPIO)"
#endif

// ============================================================================
// Common: tick + lifecycle
// ============================================================================

static void tick_cb(void *arg)
{
    static uint32_t tick = 0;
    tick++;

    if (!s_runtime_enabled) {
        backend_force_off();
        return;
    }
    render(compute_state(), tick);
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
    if (!enabled) {
        // Force off immediately so the user sees the toggle take effect
        // without waiting for the next tick.
        backend_force_off();
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

    esp_err_t err = backend_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s init failed (gpio=%d): %s — running without LED",
                 backend_name(), DD_LED_GPIO, esp_err_to_name(err));
        return ESP_OK;  // non-fatal
    }

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

    ESP_LOGI(TAG, "%s on GPIO%d, brightness=%d, runtime=%s",
             backend_name(), DD_LED_GPIO, DD_LED_BRIGHTNESS,
             s_runtime_enabled ? "on" : "off");
    return ESP_OK;
}

#else   /* DD_LED_ENABLE == 0 */

esp_err_t dd_led_init(void) { return ESP_OK; }
esp_err_t dd_led_set_enabled(bool enabled) { (void)enabled; return ESP_OK; }
bool dd_led_is_enabled(void) { return false; }

#endif  /* DD_LED_ENABLE */
