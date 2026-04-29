#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Stub for one release returned by GitHub API.
typedef struct {
    char tag[40];                  // "v0.7.0"
    char published_at[32];         // ISO8601 e.g. "2026-04-29T12:00:00Z"
    char asset_name[64];           // "dingdong.bin"
    char asset_url[256];           // browser_download_url
    char sha256_hex[65];           // 64 hex chars + NUL (from `digest` field, "sha256:" prefix stripped)
    int  asset_bytes;
} dd_ota_release_t;

// GitHub repo to pull from. Can be overridden at build time:
//   make build OTA_REPO=youruser/yourrepo
#ifndef DD_OTA_REPO
#define DD_OTA_REPO "chinaboard/dingdong"
#endif

// Asset filename matching. CI uploads `dingdong-{version}.bin` (the OTA
// app binary), `dingdong-merged-{version}.bin` (bootloader+app combined —
// must NOT be flashed to OTA slot), bootloader.bin, partition-table.bin,
// ota_data_initial.bin. We pick: starts with PREFIX, ends with ".bin",
// and does NOT contain BLOCKED.
#ifndef DD_OTA_ASSET_PREFIX
#define DD_OTA_ASSET_PREFIX "dingdong-"
#endif
#ifndef DD_OTA_ASSET_BLOCKED
#define DD_OTA_ASSET_BLOCKED "merged"
#endif

// Fetch /repos/{repo}/releases/latest from GitHub API. Fills `out` if successful.
// HTTPS without cert verification (see component README); integrity provided
// by SHA256 verification at OTA-pull time, not by transport.
esp_err_t dd_ota_check(dd_ota_release_t *out);

// Download asset_url, verify SHA256 against sha256_hex, then activate as next
// boot partition. On success the caller should restart the device. On failure
// (network, hash mismatch, OTA write error) the partition is left in its
// previous state — safe to retry.
esp_err_t dd_ota_pull(const dd_ota_release_t *r);

#ifdef __cplusplus
}
#endif
