#include "dd_session.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "session";

#define MAX_SESSIONS 4
#define TTL_USEC     (7LL * 24 * 3600 * 1000000LL)

typedef struct {
    char     token[DD_SESSION_TOKEN_LEN];
    int64_t  expires_us;  // 0 = empty slot
} slot_t;

static slot_t s_slots[MAX_SESSIONS];
static SemaphoreHandle_t s_lock;

static void hex_encode(const uint8_t *in, size_t in_len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len; ++i) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[in_len * 2] = '\0';
}

esp_err_t dd_session_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(s_slots, 0, sizeof(s_slots));
    return ESP_OK;
}

static int find_slot_locked(int64_t now)
{
    int free_idx = -1;
    int oldest_idx = 0;
    int64_t oldest_exp = INT64_MAX;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (s_slots[i].expires_us == 0 || s_slots[i].expires_us < now) {
            if (free_idx < 0) free_idx = i;
        }
        if (s_slots[i].expires_us < oldest_exp) {
            oldest_exp = s_slots[i].expires_us;
            oldest_idx = i;
        }
    }
    return free_idx >= 0 ? free_idx : oldest_idx;
}

esp_err_t dd_session_create(char *token_out, size_t cap)
{
    if (!token_out || cap < DD_SESSION_TOKEN_LEN) return ESP_ERR_INVALID_ARG;

    uint8_t bytes[32];
    esp_fill_random(bytes, sizeof(bytes));

    char token[DD_SESSION_TOKEN_LEN];
    hex_encode(bytes, sizeof(bytes), token);

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_slot_locked(now);
    memcpy(s_slots[idx].token, token, DD_SESSION_TOKEN_LEN);
    s_slots[idx].expires_us = now + TTL_USEC;
    xSemaphoreGive(s_lock);

    memcpy(token_out, token, DD_SESSION_TOKEN_LEN);
    ESP_LOGI(TAG, "session created (slot=%d, expires_in=%lldh)",
             idx, (long long)(TTL_USEC / 3600 / 1000000));
    return ESP_OK;
}

bool dd_session_is_valid(const char *token)
{
    if (!token || strlen(token) != DD_SESSION_TOKEN_LEN - 1) return false;
    int64_t now = esp_timer_get_time();
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (s_slots[i].expires_us > now &&
            memcmp(s_slots[i].token, token, DD_SESSION_TOKEN_LEN - 1) == 0) {
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void dd_session_destroy(const char *token)
{
    if (!token) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (s_slots[i].expires_us != 0 &&
            memcmp(s_slots[i].token, token, DD_SESSION_TOKEN_LEN - 1) == 0) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            ESP_LOGI(TAG, "session destroyed (slot=%d)", i);
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

void dd_session_destroy_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_slots, 0, sizeof(s_slots));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "all sessions destroyed");
}
