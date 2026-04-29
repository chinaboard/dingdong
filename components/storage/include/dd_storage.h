#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dd_storage_init(void);

// Mount point and event log path.
#define DD_STORAGE_MOUNT  "/storage"
#define DD_STORAGE_EVENTS DD_STORAGE_MOUNT "/events.jsonl"

// Append a single JSON event line. The function adds a trailing '\n';
// caller must NOT include one.
//
// Auto-rotates the events file when it exceeds DD_EVENTS_MAX_BYTES (safety
// net). Time-based rotation runs periodically and trims events older than
// DD_EVENTS_RETENTION_DAYS, which is the primary size control. The cap sits
// well under the storage partition (896 KB) to leave LittleFS metadata room.
#define DD_EVENTS_MAX_BYTES        (512 * 1024)
#define DD_EVENTS_RETENTION_DAYS   90    // ~3 months
esp_err_t dd_storage_event_append(const char *json_line);

// Drop events with ts < cutoff_unix. Caller controls the cutoff (typically
// now - retention seconds). No-op if file empty or all events newer.
esp_err_t dd_storage_event_rotate_by_time(int64_t cutoff_unix);

// Number of newline-terminated lines currently in the events file.
size_t    dd_storage_event_count(void);

// Number of bytes in the events file.
size_t    dd_storage_event_bytes(void);

// Iterate all events, calling cb(line, arg) per line. cb returns ESP_OK to
// continue, anything else to stop early. Lines are NUL-terminated and do not
// include the trailing '\n'.
typedef esp_err_t (*dd_event_iter_cb)(const char *line, void *arg);
esp_err_t dd_storage_event_iter(dd_event_iter_cb cb, void *arg);

// Wipe the events file. Returns ESP_OK if removed or didn't exist.
esp_err_t dd_storage_event_wipe(void);

// Drop all events whose worker_id field equals `id`. Used when permanently
// deleting a worker so historical attribution doesn't leak.
esp_err_t dd_storage_event_delete_by_worker(unsigned worker_id);

// Delete a single event identified by its (ts, mono_us) tuple. The pair is
// unique within the file: ts = wall-clock unix seconds, mono_us = device
// monotonic time at recording, both written verbatim by dd_event_record.
// No-op if no row matches. Caller must already know the values from a
// prior /api/events fetch.
esp_err_t dd_storage_event_delete_one(int64_t ts, int64_t mono_us);

// LittleFS partition info. Either pointer may be NULL.
esp_err_t dd_storage_fs_info(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif
