#include "dd_event.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "dd_storage.h"
#include "dd_time.h"
#include "dd_worker.h"

static const char *TAG = "event";

#define DEBOUNCE_US (10LL * 1000 * 1000)  // 10 s

// Per-peer last-write tracking. Tiny LRU; for >4 distinct peers oldest evicted.
typedef struct {
    uint8_t addr[6];
    int64_t last_in_mono;
    int64_t last_out_mono;
    bool    used;
} debounce_slot_t;

#define DEBOUNCE_SLOTS 4
static debounce_slot_t s_debounce[DEBOUNCE_SLOTS];
static SemaphoreHandle_t s_debounce_lock = NULL;

static debounce_slot_t *get_slot(const uint8_t addr[6])
{
    int free_idx = -1;
    int oldest_idx = 0;
    int64_t oldest_ts = INT64_MAX;
    for (int i = 0; i < DEBOUNCE_SLOTS; ++i) {
        if (!s_debounce[i].used) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (memcmp(s_debounce[i].addr, addr, 6) == 0) {
            return &s_debounce[i];
        }
        int64_t most_recent = s_debounce[i].last_in_mono > s_debounce[i].last_out_mono
                              ? s_debounce[i].last_in_mono : s_debounce[i].last_out_mono;
        if (most_recent < oldest_ts) {
            oldest_ts = most_recent;
            oldest_idx = i;
        }
    }
    int idx = free_idx >= 0 ? free_idx : oldest_idx;
    memset(&s_debounce[idx], 0, sizeof(s_debounce[idx]));
    memcpy(s_debounce[idx].addr, addr, 6);
    s_debounce[idx].used = true;
    return &s_debounce[idx];
}

const char *dd_event_type_str(dd_event_type_t t)
{
    switch (t) {
    case DD_EV_IN:  return "in";
    case DD_EV_OUT: return "out";
    default:        return "?";
    }
}

const char *dd_event_source_str(dd_event_source_t s)
{
    switch (s) {
    case DD_SRC_BLE_AUTO:    return "ble_auto";
    case DD_SRC_MANUAL_WEB:  return "manual_web";
    case DD_SRC_MANUAL_BTN:  return "manual_btn";
    default:                 return "?";
    }
}

esp_err_t dd_event_record(dd_event_type_t type, dd_event_source_t src,
                          const uint8_t peer_addr[6], int reason,
                          const char *note)
{
    int64_t mono = dd_time_mono_us();
    int64_t ts   = dd_time_now_unix();   // 0 if NTP not synced

    // Lazy-init debounce mutex (callers may come from BLE host or HTTP task).
    if (!s_debounce_lock) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        // Race-safe: only the first creator wins; others discard their copy.
        if (!__sync_bool_compare_and_swap(&s_debounce_lock, NULL, m)) {
            vSemaphoreDelete(m);
        }
    }

    // Debounce only BLE_AUTO source — manual web clicks are intentional and
    // shouldn't be silently dropped on double-click. Source-aware skip.
    if (peer_addr && src == DD_SRC_BLE_AUTO) {
        xSemaphoreTake(s_debounce_lock, portMAX_DELAY);
        debounce_slot_t *slot = get_slot(peer_addr);
        int64_t *last = (type == DD_EV_IN) ? &slot->last_in_mono : &slot->last_out_mono;
        if (*last != 0 && (mono - *last) < DEBOUNCE_US) {
            int64_t delta_ms = (mono - *last) / 1000;
            xSemaphoreGive(s_debounce_lock);
            ESP_LOGI(TAG, "debounced %s for %02x:%02x:%02x:%02x:%02x:%02x (Δ=%lldms)",
                     dd_event_type_str(type),
                     peer_addr[0], peer_addr[1], peer_addr[2],
                     peer_addr[3], peer_addr[4], peer_addr[5],
                     (long long)delta_ms);
            return ESP_OK;
        }
        *last = mono;
        xSemaphoreGive(s_debounce_lock);
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "ts",      (double)ts);
    cJSON_AddNumberToObject(j, "mono_us", (double)mono);
    cJSON_AddStringToObject(j, "type",    dd_event_type_str(type));
    cJSON_AddStringToObject(j, "src",     dd_event_source_str(src));

    uint16_t worker_id = 0;
    if (peer_addr) {
        worker_id = dd_worker_lookup_or_create(peer_addr);
        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 peer_addr[0], peer_addr[1], peer_addr[2],
                 peer_addr[3], peer_addr[4], peer_addr[5]);
        cJSON_AddStringToObject(j, "peer", addr_str);
    }
    if (worker_id != 0) {
        cJSON_AddNumberToObject(j, "worker_id", worker_id);
    }
    if (type == DD_EV_OUT) {
        cJSON_AddNumberToObject(j, "reason", reason);
    }
    if (note && note[0]) {
        cJSON_AddStringToObject(j, "note", note);
    }

    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return ESP_ERR_NO_MEM;

    esp_err_t err = dd_storage_event_append(s);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s ts=%lld src=%s peer=%s",
                 dd_event_type_str(type), (long long)ts,
                 dd_event_source_str(src), s);
    }
    free(s);
    return err;
}
