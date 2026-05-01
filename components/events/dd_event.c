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

esp_err_t dd_event_init(void)
{
    s_debounce_lock = xSemaphoreCreateMutex();
    return s_debounce_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

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

// Bulk: scan events.jsonl ONCE, per line look up addr in peers[] and update
// each peer's latest. "Latest" prefers (ts, mono_us); falls back to file
// order when ts is missing/zero (early events recorded before NTP sync).
typedef struct {
    int64_t  best_ts;
    int64_t  best_mono;
    int      best_idx;
    bool     found;
    dd_event_type_t type;
    char     addr_str[18];   // pre-formatted for fast strcmp
} peer_track_t;

typedef struct {
    peer_track_t *track;
    int           n;
    int           cur_idx;
} bulk_ctx_t;

static esp_err_t bulk_iter_cb(const char *line, void *arg)
{
    bulk_ctx_t *c = arg;
    c->cur_idx++;

    cJSON *j = cJSON_Parse(line);
    if (!j) return ESP_OK;

    const cJSON *peer  = cJSON_GetObjectItem(j, "peer");
    const cJSON *typej = cJSON_GetObjectItem(j, "type");
    if (!cJSON_IsString(peer) || !cJSON_IsString(typej)) {
        cJSON_Delete(j); return ESP_OK;
    }

    // Find which peer this is — early-out via memcmp on pre-formatted strs.
    int hit = -1;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->track[i].addr_str, peer->valuestring) == 0) {
            hit = i; break;
        }
    }
    if (hit < 0) { cJSON_Delete(j); return ESP_OK; }

    const cJSON *tsj   = cJSON_GetObjectItem(j, "ts");
    const cJSON *monoj = cJSON_GetObjectItem(j, "mono_us");
    int64_t ts   = cJSON_IsNumber(tsj)   ? (int64_t)tsj->valuedouble   : 0;
    int64_t mono = cJSON_IsNumber(monoj) ? (int64_t)monoj->valuedouble : 0;

    peer_track_t *t = &c->track[hit];
    bool newer = false;
    if (!t->found) {
        newer = true;
    } else if (ts > 0 && t->best_ts > 0) {
        newer = (ts > t->best_ts) || (ts == t->best_ts && mono > t->best_mono);
    } else {
        newer = c->cur_idx > t->best_idx;
    }
    if (newer) {
        t->found    = true;
        t->best_ts  = ts;
        t->best_mono= mono;
        t->best_idx = c->cur_idx;
        t->type     = (strcmp(typej->valuestring, "in") == 0) ? DD_EV_IN : DD_EV_OUT;
    }
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t dd_event_latest_for_peers(dd_event_peer_latest_t *peers, int n)
{
    if (!peers || n <= 0) return ESP_ERR_INVALID_ARG;
    if (n > 8) n = 8;

    peer_track_t track[8] = {0};
    for (int i = 0; i < n; i++) {
        snprintf(track[i].addr_str, sizeof(track[i].addr_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 peers[i].addr[0], peers[i].addr[1], peers[i].addr[2],
                 peers[i].addr[3], peers[i].addr[4], peers[i].addr[5]);
    }
    bulk_ctx_t ctx = { .track = track, .n = n, .cur_idx = 0 };
    dd_storage_event_iter(bulk_iter_cb, &ctx);

    for (int i = 0; i < n; i++) {
        peers[i].has_event = track[i].found;
        peers[i].type      = track[i].type;
    }
    return ESP_OK;
}

esp_err_t dd_event_record(dd_event_type_t type, dd_event_source_t src,
                          const uint8_t peer_addr[6], int reason,
                          const char *note)
{
    int64_t mono = dd_time_mono_us();
    int64_t ts   = dd_time_now_unix();   // 0 if NTP not synced

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
