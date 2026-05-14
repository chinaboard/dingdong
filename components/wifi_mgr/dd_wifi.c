#include "dd_wifi.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

#include "dd_config.h"
#include "dd_captive.h"
#include "mdns.h"

static const char *TAG = "wifi";

// SoftAP SSID is `dingdong-setup-XXXX` where XXXX is the last 2 bytes of the
// chip's WiFi STA MAC, computed once at start_softap() time. Keeps multiple
// devices in setup mode distinguishable to a phone scanning nearby APs.
#define AP_SSID_PREFIX  "dingdong-setup-"
#define AP_SSID_MAX     32       // WiFi SSID hard limit
#define AP_CHANNEL      6
#define AP_MAX_CONN     4
#define STA_RETRY_MAX   5

// If STA stays disconnected for 10 minutes, give up and restart so device
// gets a fresh attempt. Useful when AP comes back online.
#define STA_GIVE_UP_MS  (10LL * 60 * 1000)

static dd_wifi_state_t s_state = DD_WIFI_STATE_DOWN;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap  = NULL;
static int s_sta_retry = 0;
static int64_t s_sta_down_since_us = 0;
static esp_timer_handle_t s_sta_giveup_timer = NULL;
static bool s_mdns_started = false;

// Bring up mDNS so the device is reachable via http://dingdong-XXXX.local
// from any modern OS, no router-DHCP-table hunting needed. Idempotent —
// safe across STA reconnects. Hostname is BT-MAC-derived (matches the BLE
// name + SoftAP SSID suffix) so it stays stable across BLE display-name
// renames; bookmarks survive.
static void mdns_bring_up(void)
{
    if (s_mdns_started) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed");
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char host[24];
    snprintf(host, sizeof(host), "dingdong-%02x%02x", mac[4], mac[5]);
    mdns_hostname_set(host);
    mdns_instance_name_set("Dingdong");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS up: http://%s.local", host);
}

static void sta_giveup_cb(void *arg)
{
    if (s_state == DD_WIFI_STATE_STA_GOT_IP) return;  // recovered
    if (s_sta_down_since_us == 0) return;
    int64_t down_ms = (esp_timer_get_time() - s_sta_down_since_us) / 1000;
    if (down_ms >= STA_GIVE_UP_MS) {
        ESP_LOGE(TAG, "STA down for %lld ms, restarting device", (long long)down_ms);
        esp_restart();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = data;
            if (s_sta_down_since_us == 0) s_sta_down_since_us = esp_timer_get_time();
            if (s_sta_retry < STA_RETRY_MAX) {
                s_sta_retry++;
                ESP_LOGW(TAG, "STA disconnected, retry %d/%d (reason=%d %s)",
                         s_sta_retry, STA_RETRY_MAX, e->reason,
                         e->reason == 2   ? "AUTH_EXPIRE"           :
                         e->reason == 15  ? "4WAY_HANDSHAKE_TIMEOUT" :
                         e->reason == 200 ? "BEACON_TIMEOUT"        :
                         e->reason == 201 ? "NO_AP_FOUND"           :
                         e->reason == 202 ? "AUTH_FAIL"             :
                         e->reason == 203 ? "ASSOC_FAIL"            :
                         e->reason == 204 ? "HANDSHAKE_TIMEOUT"     :
                         e->reason == 205 ? "CONNECTION_FAIL"       :
                         e->reason == 207 ? "ROAMING"               : "?");
                esp_wifi_connect();
                s_state = DD_WIFI_STATE_STA_CONNECTING;
            } else {
                // Keep trying every 30s but reset retry counter, and arm
                // give-up timer (restarts device after 10 min total down).
                ESP_LOGE(TAG, "STA failed after %d retries — slow-retry mode",
                         STA_RETRY_MAX);
                s_state = DD_WIFI_STATE_DOWN;
                s_sta_retry = 0;
                esp_wifi_connect();  // try again, controller will rate-limit
                if (!s_sta_giveup_timer) {
                    const esp_timer_create_args_t args = {
                        .callback = &sta_giveup_cb,
                        .name = "sta_giveup",
                    };
                    esp_timer_create(&args, &s_sta_giveup_timer);
                }
                esp_timer_stop(s_sta_giveup_timer);
                esp_timer_start_periodic(s_sta_giveup_timer, 60ULL * 1000 * 1000);
            }
            break;
        }
        case WIFI_EVENT_AP_START: {
            esp_netif_ip_info_t ip;
            esp_netif_get_ip_info(s_netif_ap, &ip);
            wifi_config_t wc;
            esp_wifi_get_config(WIFI_IF_AP, &wc);
            ESP_LOGI(TAG, "SoftAP up: ssid=%s gw=" IPSTR, (char *)wc.ap.ssid, IP2STR(&ip.gw));
            // Captive portal: DNS hijack so phones auto-pop the setup page.
            dd_captive_start(ip.gw.addr);
            s_state = DD_WIFI_STATE_AP;
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "AP client joined: " MACSTR " (aid=%d)",
                     MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "AP client left: " MACSTR " (aid=%d, reason=%d)",
                     MAC2STR(e->mac), e->aid, e->reason);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_sta_retry = 0;
        s_sta_down_since_us = 0;
        if (s_sta_giveup_timer) esp_timer_stop(s_sta_giveup_timer);
        s_state = DD_WIFI_STATE_STA_GOT_IP;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        mdns_bring_up();
    }
}

static esp_err_t start_softap(void)
{
    s_netif_ap = esp_netif_create_default_wifi_ap();
    // STA netif also created so esp_wifi_scan_start can run while AP is up.
    s_netif_sta = esp_netif_create_default_wifi_sta();

    // Build per-device SSID `dingdong-setup-XXXX` (or `dingdong-rec-XXXX`
    // in recovery mode) from the BT MAC last 2 bytes. Using BT MAC (not
    // WiFi MAC) so the suffix matches the BLE device name `dingdong-XXXX`
    // — one identifier per physical device.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char ssid[AP_SSID_MAX];
    const char *prefix = dd_metrics_in_recovery_mode()
        ? "dingdong-rec-"   // boot-loop recovery — visible at a glance
        : AP_SSID_PREFIX;
    int slen = snprintf(ssid, sizeof(ssid), "%s%02X%02X",
                        prefix, mac[4], mac[5]);

    wifi_config_t cfg = {
        .ap = {
            .channel        = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    memcpy(cfg.ap.ssid, ssid, slen);
    cfg.ap.ssid_len = slen;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static esp_err_t start_sta(void)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();

    char ssid[DD_WIFI_SSID_MAX] = {0};
    char pass[DD_WIFI_PASS_MAX] = {0};
    esp_err_t err = dd_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no wifi creds in NVS despite NORMAL boot mode");
        return err;
    }
    ESP_LOGI(TAG, "STA target ssid=%s", ssid);

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    // PMF capable lets us auto-pick SAE on WPA2/3 mixed APs. We hit a
    // U7-Pro Wi-Fi 7 AP where pure WPA2 (no SAE) silently fails the EAPOL
    // 4-way handshake; with PMF capable + SAE we go through cleanly.
    // required=false so we still join old WPA2-only APs.
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;

    s_state = DD_WIFI_STATE_STA_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Country code CN unlocks 2.4 GHz channels 12–13 (default "01" only allows
    // 1–11). Best-effort — some IDF builds gate this behind Kconfig.
    esp_err_t cc_err = esp_wifi_set_country_code("CN", true);
    if (cc_err != ESP_OK) {
        ESP_LOGW(TAG, "set_country_code(CN) rc=%s", esp_err_to_name(cc_err));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

esp_err_t dd_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    // Recovery mode forces SoftAP regardless of saved creds. See
    // DD_BOOT_LOOP_RECOVERY_THRESHOLD in dd_config.h.
    if (dd_metrics_in_recovery_mode()) {
        ESP_LOGE(TAG, "BOOT-LOOP RECOVERY: %u consecutive failures → forced SoftAP",
                 (unsigned)dd_metrics_boot_loop_count());
        return start_softap();
    }

    dd_boot_mode_t mode = dd_config_boot_mode();
    if (mode == DD_BOOT_NORMAL) {
        return start_sta();
    } else {
        ESP_LOGI(TAG, "boot mode %s → SoftAP", dd_boot_mode_str(mode));
        return start_softap();
    }
}

dd_wifi_state_t dd_wifi_state(void) { return s_state; }

const char *dd_wifi_state_str(dd_wifi_state_t s)
{
    switch (s) {
    case DD_WIFI_STATE_DOWN:           return "DOWN";
    case DD_WIFI_STATE_AP:             return "AP";
    case DD_WIFI_STATE_STA_CONNECTING: return "STA_CONNECTING";
    case DD_WIFI_STATE_STA_GOT_IP:     return "STA_GOT_IP";
    default:                           return "?";
    }
}

void dd_wifi_get_ip(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    esp_netif_t *nif = NULL;
    if (s_state == DD_WIFI_STATE_AP) nif = s_netif_ap;
    else if (s_state == DD_WIFI_STATE_STA_GOT_IP) nif = s_netif_sta;
    if (!nif) return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
        const esp_ip4_addr_t *addr = (s_state == DD_WIFI_STATE_AP) ? &ip.gw : &ip.ip;
        snprintf(out, cap, IPSTR, IP2STR(addr));
    }
}
