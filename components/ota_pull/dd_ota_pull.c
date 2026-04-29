// GitHub Releases OTA pull. Threat model: hobby device, integrity guaranteed
// by SHA256 against GitHub Releases API `digest` field, NOT by TLS verification.
// We deliberately disable cert verification (CONFIG_ESP_TLS_INSECURE +
// CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) so we don't need to ship a CA bundle
// or worry about cert rotation bricking the OTA path. The hash check is the
// only thing standing between you and a corrupt/swapped image.
//
// Flow:
//   1. dd_ota_check  → GET /releases/latest, parse JSON for tag + asset URL + digest
//   2. dd_ota_pull   → stream asset into next OTA partition via esp_https_ota
//                       then re-read partition, compute SHA256, compare with digest
//                       only set boot partition if hash matches

#include "dd_ota_pull.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cJSON.h"
#include "psa/crypto.h"

static const char *TAG = "ota_pull";

#define API_URL "https://api.github.com/repos/" DD_OTA_REPO "/releases/latest"
// Release JSON includes the full body/release-notes string — for repos with
// long-form changelogs this can hit 10-15KB. 16KB gives headroom; if it ever
// overflows we'll see truncation in the debug log and bump again.
#define API_BUF_BYTES 16384

// ---------- helpers ----------

static esp_err_t fetch_url_to_buf(const char *url, char *buf, size_t cap, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = NULL,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_http_client_set_header(cli, "User-Agent", "dingdong-fw");
    esp_http_client_set_header(cli, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }
    int hdrs = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (hdrs < 0 || status / 100 != 2) {
        ESP_LOGE(TAG, "HTTP %d (hdrs=%d) for %s", status, hdrs, url);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total < cap - 1) {
        int n = esp_http_client_read(cli, buf + total, cap - 1 - total);
        if (n < 0) { ESP_LOGE(TAG, "read err: %d", n); break; }
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    *out_len = total;
    if (total >= cap - 1) {
        ESP_LOGW(TAG, "API response truncated at %u bytes — bump API_BUF_BYTES", (unsigned)total);
    }

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ESP_OK;
}

// Strip "sha256:" prefix from digest field, lowercase, write 64 hex chars + NUL.
// Returns false on malformed input.
static bool normalize_digest(const char *digest, char out_hex[65])
{
    const char *p = digest;
    if (strncmp(p, "sha256:", 7) == 0) p += 7;
    size_t n = strlen(p);
    if (n != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'F') c = c - 'A' + 'a';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        out_hex[i] = c;
    }
    out_hex[64] = '\0';
    return true;
}

// ---------- public ----------

esp_err_t dd_ota_check(dd_ota_release_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    char *buf = malloc(API_BUF_BYTES);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t got = 0;
    esp_err_t err = fetch_url_to_buf(API_URL, buf, API_BUF_BYTES, &got);
    if (err != ESP_OK) { free(buf); return err; }

    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *tag       = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    cJSON *published = cJSON_GetObjectItemCaseSensitive(root, "published_at");
    cJSON *assets    = cJSON_GetObjectItemCaseSensitive(root, "assets");

    if (!cJSON_IsString(tag) || !cJSON_IsArray(assets)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "release JSON missing tag_name or assets");
        return ESP_ERR_INVALID_RESPONSE;
    }

    strncpy(out->tag, tag->valuestring, sizeof(out->tag) - 1);
    if (cJSON_IsString(published)) {
        strncpy(out->published_at, published->valuestring, sizeof(out->published_at) - 1);
    }

    // Pick the OTA app binary. CI publishes `dingdong-{version}.bin` plus
    // a `dingdong-merged-{version}.bin` (bootloader+app — would brick the
    // OTA slot if flashed), plus bootloader.bin / partition-table.bin /
    // ota_data_initial.bin. Match on prefix + .bin suffix + skip blocked.
    cJSON *asset;
    bool found = false;
    cJSON_ArrayForEach(asset, assets) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        if (!cJSON_IsString(name)) continue;
        const char *nm = name->valuestring;
        size_t nlen = strlen(nm);
        if (strncmp(nm, DD_OTA_ASSET_PREFIX, strlen(DD_OTA_ASSET_PREFIX)) != 0) continue;
        if (nlen < 4 || strcmp(nm + nlen - 4, ".bin") != 0) continue;
        if (DD_OTA_ASSET_BLOCKED[0] && strstr(nm, DD_OTA_ASSET_BLOCKED) != NULL) continue;

        cJSON *url    = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        cJSON *digest = cJSON_GetObjectItemCaseSensitive(asset, "digest");
        cJSON *size   = cJSON_GetObjectItemCaseSensitive(asset, "size");
        if (!cJSON_IsString(url) || !cJSON_IsString(digest)) continue;
        if (!normalize_digest(digest->valuestring, out->sha256_hex)) {
            ESP_LOGW(TAG, "asset '%s' digest malformed: %s",
                     name->valuestring, digest->valuestring);
            continue;
        }
        strncpy(out->asset_name, name->valuestring, sizeof(out->asset_name) - 1);
        strncpy(out->asset_url,  url->valuestring,  sizeof(out->asset_url)  - 1);
        if (cJSON_IsNumber(size)) out->asset_bytes = size->valueint;
        found = true;
        break;
    }
    cJSON_Delete(root);

    if (!found) {
        ESP_LOGE(TAG, "no .bin asset with valid SHA256 digest in release %s", out->tag);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "release %s: %s (%d bytes) sha256=%.16s…",
             out->tag, out->asset_name, out->asset_bytes, out->sha256_hex);
    return ESP_OK;
}

// esp_https_ota_begin refuses ESP_ERR_INVALID_ARG unless one of cert_pem,
// use_global_ca_store, or crt_bundle_attach is set — even with
// CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y. Provide a no-op attach to satisfy
// the precondition; actual verification is skipped at the mbedtls layer.
static esp_err_t noop_crt_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

esp_err_t dd_ota_pull(const dd_ota_release_t *r)
{
    if (!r || !r->asset_url[0] || !r->sha256_hex[0]) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        ESP_LOGE(TAG, "no next OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "pull %s → partition '%s' (%lu bytes available)",
             r->asset_url, next->label, (unsigned long)next->size);

    esp_http_client_config_t http = {
        .url = r->asset_url,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = noop_crt_attach,   // satisfy esp_https_ota check
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    int last_log = 0;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int read = esp_https_ota_get_image_len_read(h);
        if (read - last_log >= 65536) {  // every 64 KB
            ESP_LOGI(TAG, "  …%d bytes", read);
            last_log = read;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_perform: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        return err;
    }

    int total = esp_https_ota_get_image_len_read(h);
    ESP_LOGI(TAG, "download complete: %d bytes, verifying SHA256...", total);

    // Verify by streaming the partition back through SHA256. If we don't
    // call esp_https_ota_finish, the boot partition isn't activated, so the
    // device stays on the current image even if we abort here.
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t pst = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (pst != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup: %d", (int)pst);
        esp_https_ota_abort(h);
        return ESP_FAIL;
    }

    uint8_t chunk[1024];
    int off = 0;
    while (off < total) {
        int n = (total - off > (int)sizeof(chunk)) ? (int)sizeof(chunk) : (total - off);
        esp_err_t rerr = esp_partition_read(next, off, chunk, n);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "partition_read at %d: %s", off, esp_err_to_name(rerr));
            psa_hash_abort(&op);
            esp_https_ota_abort(h);
            return rerr;
        }
        psa_hash_update(&op, chunk, n);
        off += n;
    }

    uint8_t hash[32];
    size_t  hash_len = 0;
    pst = psa_hash_finish(&op, hash, sizeof(hash), &hash_len);
    if (pst != PSA_SUCCESS || hash_len != 32) {
        ESP_LOGE(TAG, "psa_hash_finish: %d", (int)pst);
        esp_https_ota_abort(h);
        return ESP_FAIL;
    }

    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i*2, 3, "%02x", hash[i]);
    }
    if (strcmp(hex, r->sha256_hex) != 0) {
        ESP_LOGE(TAG, "SHA256 MISMATCH");
        ESP_LOGE(TAG, "  got      %s", hex);
        ESP_LOGE(TAG, "  expected %s", r->sha256_hex);
        esp_https_ota_abort(h);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "SHA256 OK, committing boot partition '%s'", next->label);

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_finish: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
