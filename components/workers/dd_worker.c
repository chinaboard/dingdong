#include "dd_worker.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "worker";

#define NS                "dd_workers"
#define KEY_NEXT_ID       "next_id"

// In-memory cache. Loaded once at boot from NVS; updates write through.
static dd_worker_t       s_workers[DD_WORKER_MAX];
static size_t            s_count = 0;
static uint16_t          s_next_id = 1;
static SemaphoreHandle_t s_lock;

static void key_for_id(uint16_t id, char *out, size_t cap)
{
    snprintf(out, cap, "w_%04u", id);
}

static esp_err_t load_one(nvs_handle_t h, uint16_t id, dd_worker_t *out)
{
    char k[16];
    key_for_id(id, k, sizeof(k));

    size_t sz = 0;
    esp_err_t err = nvs_get_blob(h, k, NULL, &sz);
    if (err != ESP_OK) return err;
    if (sz == 0 || sz > 1024) return ESP_ERR_INVALID_SIZE;

    char *buf = malloc(sz + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    err = nvs_get_blob(h, k, buf, &sz);
    if (err != ESP_OK) { free(buf); return err; }
    buf[sz] = '\0';

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->id = id;

    const cJSON *jaddr = cJSON_GetObjectItem(j, "addr");
    if (cJSON_IsString(jaddr)) {
        unsigned int a[6];
        if (sscanf(jaddr->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            for (int i = 0; i < 6; i++) out->addr[i] = (uint8_t)a[i];
        }
    }
    const cJSON *jname = cJSON_GetObjectItem(j, "name");
    if (cJSON_IsString(jname)) {
        strncpy(out->name, jname->valuestring, DD_WORKER_NAME_MAX - 1);
    }
    const cJSON *jcat = cJSON_GetObjectItem(j, "category");
    if (cJSON_IsString(jcat)) {
        strncpy(out->category, jcat->valuestring, DD_WORKER_CATEGORY_MAX - 1);
    }
    const cJSON *jct = cJSON_GetObjectItem(j, "created_at");
    if (cJSON_IsNumber(jct)) out->created_at = (int64_t)jct->valuedouble;

    cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t save_one(nvs_handle_t h, const dd_worker_t *w)
{
    char k[16];
    key_for_id(w->id, k, sizeof(k));

    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             w->addr[0], w->addr[1], w->addr[2], w->addr[3], w->addr[4], w->addr[5]);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "addr", addr_str);
    cJSON_AddStringToObject(j, "name", w->name);
    cJSON_AddStringToObject(j, "category", w->category);
    cJSON_AddNumberToObject(j, "created_at", (double)w->created_at);

    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_set_blob(h, k, s, strlen(s));
    free(s);
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
}

esp_err_t dd_worker_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t next = 1;
    err = nvs_get_u16(h, KEY_NEXT_ID, &next);
    if (err == ESP_ERR_NVS_NOT_FOUND) next = 1;
    s_next_id = next;

    s_count = 0;
    for (uint16_t id = 1; id < s_next_id && s_count < DD_WORKER_MAX; id++) {
        if (load_one(h, id, &s_workers[s_count]) == ESP_OK) {
            s_count++;
        }
    }
    nvs_close(h);

    ESP_LOGI(TAG, "loaded %u workers (next_id=%u)", (unsigned)s_count, s_next_id);
    return ESP_OK;
}

uint16_t dd_worker_find_by_addr(const uint8_t addr[6])
{
    if (!addr) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t out = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (memcmp(s_workers[i].addr, addr, 6) == 0) {
            out = s_workers[i].id;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return out;
}

uint16_t dd_worker_lookup_or_create(const uint8_t addr[6])
{
    if (!addr) return 0;

    uint16_t existing = dd_worker_find_by_addr(addr);
    if (existing != 0) return existing;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_count >= DD_WORKER_MAX) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "table full (%u slots), can't create", (unsigned)s_count);
        return 0;
    }

    dd_worker_t *w = &s_workers[s_count];
    memset(w, 0, sizeof(*w));
    w->id = s_next_id;
    memcpy(w->addr, addr, 6);
    snprintf(w->name, sizeof(w->name), "Unnamed_%04x%02x", s_next_id, addr[5]);
    w->category[0] = '\0';
    w->created_at = (int64_t)time(NULL);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "nvs_open in create: %s", esp_err_to_name(err));
        return 0;
    }
    err = save_one(h, w);
    if (err == ESP_OK) {
        s_next_id++;
        s_count++;
        nvs_set_u16(h, KEY_NEXT_ID, s_next_id);
        nvs_commit(h);
    }
    nvs_close(h);
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_one: %s", esp_err_to_name(err));
        return 0;
    }

    ESP_LOGI(TAG, "created worker id=%u addr=%02x:%02x:%02x:%02x:%02x:%02x",
             w->id, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return w->id;
}

esp_err_t dd_worker_get(uint16_t id, dd_worker_t *out)
{
    if (!out || id == 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < s_count; i++) {
        if (s_workers[i].id == id) {
            *out = s_workers[i];
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t dd_worker_update(uint16_t id, const char *name, const char *category)
{
    if (id == 0) return ESP_ERR_INVALID_ARG;

    // Reject empty / whitespace-only name updates — keeps lists readable.
    if (name) {
        const char *p = name;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    dd_worker_t *w = NULL;
    for (size_t i = 0; i < s_count; i++) {
        if (s_workers[i].id == id) { w = &s_workers[i]; break; }
    }
    if (!w) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (name) {
        memset(w->name, 0, sizeof(w->name));
        strncpy(w->name, name, sizeof(w->name) - 1);
    }
    if (category) {
        memset(w->category, 0, sizeof(w->category));
        strncpy(w->category, category, sizeof(w->category) - 1);
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = save_one(h, w);
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t dd_worker_delete(uint16_t id)
{
    if (id == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (size_t i = 0; i < s_count; i++) {
        if (s_workers[i].id == id) { idx = (int)i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    // Erase from NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char k[16];
        key_for_id(id, k, sizeof(k));
        nvs_erase_key(h, k);  // ignore "not found"
        err = nvs_commit(h);
        nvs_close(h);
    }

    // Compact in-RAM cache (preserve order isn't important; swap last in)
    if (idx != (int)s_count - 1) {
        s_workers[idx] = s_workers[s_count - 1];
    }
    memset(&s_workers[s_count - 1], 0, sizeof(s_workers[0]));
    s_count--;

    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "worker id=%u deleted", id);
    return err;
}

esp_err_t dd_worker_iter(dd_worker_iter_cb cb, void *arg)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < s_count; i++) {
        err = cb(&s_workers[i], arg);
        if (err != ESP_OK) break;
    }
    xSemaphoreGive(s_lock);
    return err == ESP_OK ? ESP_OK : err;
}

size_t dd_worker_count(void)
{
    return s_count;
}

esp_err_t dd_worker_restore(const dd_worker_t *w)
{
    if (!w || w->id == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Find existing slot or take new one
    int slot = -1;
    for (size_t i = 0; i < s_count; i++) {
        if (s_workers[i].id == w->id) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count >= DD_WORKER_MAX) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
        slot = s_count++;
    }
    s_workers[slot] = *w;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { xSemaphoreGive(s_lock); return err; }
    err = save_one(h, w);
    if (err == ESP_OK && w->id >= s_next_id) {
        s_next_id = w->id + 1;
        nvs_set_u16(h, KEY_NEXT_ID, s_next_id);
        nvs_commit(h);
    }
    nvs_close(h);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t dd_worker_wipe(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { xSemaphoreGive(s_lock); return err; }
    // Erase all worker keys
    for (uint16_t id = 1; id < s_next_id; id++) {
        char k[16];
        snprintf(k, sizeof(k), "w_%04u", id);
        nvs_erase_key(h, k);
    }
    nvs_erase_key(h, KEY_NEXT_ID);
    nvs_commit(h);
    nvs_close(h);
    s_count = 0;
    s_next_id = 1;
    memset(s_workers, 0, sizeof(s_workers));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "workers wiped");
    return ESP_OK;
}
