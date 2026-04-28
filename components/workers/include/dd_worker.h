#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DD_WORKER_NAME_MAX     32
#define DD_WORKER_CATEGORY_MAX 16
#define DD_WORKER_MAX          32  // hard cap; revoked workers still occupy slots
                                   // until factory reset (kept for history lookup)

typedef struct {
    uint16_t id;                                 // 1-based, 0 = unset/invalid
    uint8_t  addr[6];                            // BLE peer identity address
    char     name[DD_WORKER_NAME_MAX];
    char     category[DD_WORKER_CATEGORY_MAX];
    int64_t  created_at;
    bool     revoked;
} dd_worker_t;

esp_err_t dd_worker_init(void);

// Find worker by BLE address. Returns 0 if not found.
uint16_t  dd_worker_find_by_addr(const uint8_t addr[6]);

// Find or create. Auto-assigns next id and stub name. Returns 0 on failure
// (e.g. table full).
uint16_t  dd_worker_lookup_or_create(const uint8_t addr[6]);

esp_err_t dd_worker_get(uint16_t id, dd_worker_t *out);

// Update name/category (and clear revoked) for an existing worker.
esp_err_t dd_worker_update(uint16_t id, const char *name, const char *category);

// Hard-delete the worker: remove the slot from NVS + RAM cache so the id
// becomes free for reuse. Caller is responsible for separately scrubbing the
// BLE bond and any historical events that reference this worker_id (those
// would otherwise show "id=N (unknown)" in views).
esp_err_t dd_worker_delete(uint16_t id);

// Iterate all workers (including revoked). cb returns ESP_OK to continue.
typedef esp_err_t (*dd_worker_iter_cb)(const dd_worker_t *w, void *arg);
esp_err_t dd_worker_iter(dd_worker_iter_cb cb, void *arg);

size_t    dd_worker_count(void);

// Restore (seed) a worker bypassing auto-id assignment. Used by
// /api/system/restore. Caller provides full struct including id. Existing
// worker with same id is overwritten; if id is beyond current next_id,
// next_id advances. Returns ESP_OK on success.
esp_err_t dd_worker_restore(const dd_worker_t *w);

// Wipe all workers + reset next_id. For restore flow.
esp_err_t dd_worker_wipe(void);

#ifdef __cplusplus
}
#endif
