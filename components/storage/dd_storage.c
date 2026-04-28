#include "dd_storage.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "storage";

#define PARTITION_LABEL "storage"
#define LINE_BUF_MAX    1024

static SemaphoreHandle_t s_lock;
static volatile size_t   s_event_count = 0;   // cached; updated on append/rotate

static size_t scan_event_count(void)
{
    FILE *f = fopen(DD_STORAGE_EVENTS, "r");
    if (!f) return 0;
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

esp_err_t dd_storage_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_vfs_littlefs_conf_t conf = {
        .base_path = DD_STORAGE_MOUNT,
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: total=%u used=%u",
                 DD_STORAGE_MOUNT, (unsigned)total, (unsigned)used);
    }

    // Initial scan to populate cached count.
    s_event_count = scan_event_count();
    ESP_LOGI(TAG, "events file has %u entries", (unsigned)s_event_count);

    return ESP_OK;
}

static esp_err_t rotate_events_locked(void)
{
    // Drop oldest ~half of the file to bound size. Read from middle to end
    // (line-aligned), write to .tmp, then atomically rename over original.
    FILE *in = fopen(DD_STORAGE_EVENTS, "r");
    if (!in) return ESP_OK;

    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    long target_keep = sz / 2;
    fseek(in, sz - target_keep, SEEK_SET);

    // Skip partial line at the cut point
    int c;
    while ((c = fgetc(in)) != EOF && c != '\n') { }

    FILE *out = fopen(DD_STORAGE_EVENTS ".tmp", "w");
    if (!out) { fclose(in); return ESP_FAIL; }

    char buf[1024];
    size_t n;
    bool write_ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ESP_LOGE(TAG, "rotate fwrite short");
            write_ok = false;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) {
        ESP_LOGE(TAG, "rotate fclose failed");
        write_ok = false;
    }

    if (!write_ok) {
        // Don't replace original — keep old file intact for next attempt.
        unlink(DD_STORAGE_EVENTS ".tmp");
        return ESP_FAIL;
    }

    // POSIX rename atomically replaces target on success. Don't unlink
    // beforehand — that would lose data if rename fails (e.g. ENOSPC).
    int rc = rename(DD_STORAGE_EVENTS ".tmp", DD_STORAGE_EVENTS);
    if (rc != 0) {
        ESP_LOGE(TAG, "rotate rename errno=%d", errno);
        unlink(DD_STORAGE_EVENTS ".tmp");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "events rotated: kept %ld bytes (was %ld)",
             target_keep, sz);
    return ESP_OK;
}

esp_err_t dd_storage_event_append(const char *json_line)
{
    if (!json_line) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(json_line);
    if (len == 0 || len > LINE_BUF_MAX) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    // Check size; rotate if over cap
    struct stat st;
    if (stat(DD_STORAGE_EVENTS, &st) == 0 && st.st_size > DD_EVENTS_MAX_BYTES) {
        rotate_events_locked();
        // Rotation drops oldest ~half — recount.
        s_event_count = scan_event_count();
    }

    FILE *f = fopen(DD_STORAGE_EVENTS, "a");
    if (!f) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "fopen append failed");
        return ESP_FAIL;
    }
    size_t w1 = fwrite(json_line, 1, len, f);
    int    w2 = fputc('\n', f);
    int    fl = fflush(f);
    fclose(f);
    if (w1 == len && w2 != EOF && fl == 0) {
        s_event_count++;
    }
    xSemaphoreGive(s_lock);

    if (w1 != len || w2 == EOF || fl != 0) {
        ESP_LOGE(TAG, "fwrite event short");
        return ESP_FAIL;
    }
    return ESP_OK;
}

size_t dd_storage_event_count(void)
{
    return s_event_count;  // O(1) cached value
}

size_t dd_storage_event_bytes(void)
{
    struct stat st;
    if (stat(DD_STORAGE_EVENTS, &st) != 0) return 0;
    return (size_t)st.st_size;
}

esp_err_t dd_storage_event_iter(dd_event_iter_cb cb, void *arg)
{
    if (!cb) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(DD_STORAGE_EVENTS, "r");
    if (!f) { xSemaphoreGive(s_lock); return ESP_OK; }  // empty == OK

    char buf[LINE_BUF_MAX + 1];
    esp_err_t err = ESP_OK;
    while (fgets(buf, sizeof(buf), f)) {
        size_t l = strlen(buf);
        if (l && buf[l - 1] == '\n') buf[--l] = '\0';
        if (l == 0) continue;
        err = cb(buf, arg);
        if (err != ESP_OK) break;
    }
    fclose(f);
    xSemaphoreGive(s_lock);
    return err == ESP_OK ? ESP_OK : err;
}

esp_err_t dd_storage_event_wipe(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = unlink(DD_STORAGE_EVENTS);
    if (rc == 0 || errno == ENOENT) s_event_count = 0;
    xSemaphoreGive(s_lock);
    if (rc != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "unlink failed errno=%d", errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "events wiped");
    return ESP_OK;
}

// Cheap ts parser: events are written by dd_event_record() with `ts` as the
// FIRST field, so the line always starts with `{"ts":NUMBER,`. Anchoring on
// the prefix (vs `strstr`) means a user-supplied `note` containing the
// literal `"ts":N` substring can't fool the parser into dropping events.
static int64_t parse_ts_from_line(const char *line)
{
    if (strncmp(line, "{\"ts\":", 6) != 0) return 0;
    return (int64_t)atoll(line + 6);
}

esp_err_t dd_storage_event_rotate_by_time(int64_t cutoff_unix)
{
    if (cutoff_unix <= 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    FILE *in = fopen(DD_STORAGE_EVENTS, "r");
    if (!in) { xSemaphoreGive(s_lock); return ESP_OK; }

    // Find first line with ts >= cutoff. Track its byte offset.
    char buf[512];
    long keep_offset = -1;
    long offset_of_line = 0;
    while (fgets(buf, sizeof(buf), in)) {
        int64_t ts = parse_ts_from_line(buf);
        if (ts >= cutoff_unix) {
            keep_offset = offset_of_line;
            break;
        }
        // Account: if line was longer than buffer, fgets chunks it. For our
        // well-formed events (~80B), single fgets reads the whole line.
        offset_of_line = ftell(in);
    }

    if (keep_offset < 0) {
        // No events new enough — wipe.
        fclose(in);
        unlink(DD_STORAGE_EVENTS);
        s_event_count = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "rotate-by-time: all events older than cutoff, wiped");
        return ESP_OK;
    }
    if (keep_offset == 0) {
        // All events are within retention — nothing to do.
        fclose(in);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    // Stream-copy from keep_offset to .tmp.
    fseek(in, keep_offset, SEEK_SET);
    FILE *out = fopen(DD_STORAGE_EVENTS ".tmp", "w");
    if (!out) {
        fclose(in);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    char copy[1024];
    size_t n;
    bool ok = true;
    while ((n = fread(copy, 1, sizeof(copy), in)) > 0) {
        if (fwrite(copy, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) {
        unlink(DD_STORAGE_EVENTS ".tmp");
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    if (rename(DD_STORAGE_EVENTS ".tmp", DD_STORAGE_EVENTS) != 0) {
        unlink(DD_STORAGE_EVENTS ".tmp");
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    s_event_count = scan_event_count();
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "rotate-by-time: kept %u events from offset %ld",
             (unsigned)s_event_count, keep_offset);
    return ESP_OK;
}

esp_err_t dd_storage_fs_info(size_t *total, size_t *used)
{
    return esp_littlefs_info(PARTITION_LABEL, total, used);
}

esp_err_t dd_storage_event_delete_by_worker(unsigned worker_id)
{
    if (worker_id == 0) return ESP_ERR_INVALID_ARG;

    char needle[40];
    int needle_len = snprintf(needle, sizeof(needle), "\"worker_id\":%u", worker_id);
    if (needle_len <= 0) return ESP_FAIL;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    FILE *in = fopen(DD_STORAGE_EVENTS, "r");
    if (!in) { xSemaphoreGive(s_lock); return ESP_OK; }

    FILE *out = fopen(DD_STORAGE_EVENTS ".tmp", "w");
    if (!out) {
        fclose(in);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    char buf[LINE_BUF_MAX];
    size_t kept = 0, dropped = 0;
    bool ok = true;
    while (fgets(buf, sizeof(buf), in)) {
        // Substring match — every event for this worker has "worker_id":N
        // verbatim because cJSON_PrintUnformatted emits the field this way.
        // Bound check: only match when the digits end at a non-digit char
        // (so "worker_id":1 doesn't also match "worker_id":12 etc.).
        const char *m = strstr(buf, needle);
        bool drop = false;
        if (m) {
            char after = m[needle_len];
            if (after == ',' || after == '}' || after == ' ') {
                drop = true;
            }
        }
        if (drop) {
            dropped++;
        } else {
            if (fputs(buf, out) == EOF) { ok = false; break; }
            kept++;
        }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;

    if (!ok) {
        unlink(DD_STORAGE_EVENTS ".tmp");
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    if (dropped == 0) {
        unlink(DD_STORAGE_EVENTS ".tmp");
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    if (kept == 0) {
        unlink(DD_STORAGE_EVENTS ".tmp");
        unlink(DD_STORAGE_EVENTS);
        s_event_count = 0;
    } else {
        if (rename(DD_STORAGE_EVENTS ".tmp", DD_STORAGE_EVENTS) != 0) {
            unlink(DD_STORAGE_EVENTS ".tmp");
            xSemaphoreGive(s_lock);
            return ESP_FAIL;
        }
        s_event_count = kept;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "delete worker_id=%u: dropped %u events, kept %u",
             worker_id, (unsigned)dropped, (unsigned)kept);
    return ESP_OK;
}
