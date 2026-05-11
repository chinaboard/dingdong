#include "dd_ble.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/hid/ble_svc_hid.h"
#include "os/os_mbuf.h"
#include "nimble/nimble_npl.h"

#include "dd_event.h"
#include "dd_config.h"
#include "dd_time.h"

static const char *TAG = "ble";

// Apparance code 0x03C1 = HID Keyboard (BT GATT specs).
#define APPEARANCE_HID_KEYBOARD 0x03C1
#define HID_SERVICE_UUID16      0x1812
#define BOND_MAX_LIST           8

static char     s_device_name[20] = "dingdong-XXXX";
static uint8_t  s_own_addr_type;
static bool     s_advertising = false;
static esp_timer_handle_t s_adv_restart_timer = NULL;
static esp_timer_handle_t s_scan_restart_timer = NULL;

// ---------- presence (scan-based + connect-probe fallback) ----------
// Continuous scan resolves iPhone RPAs to identity via IRK exchanged during
// bonding. NimBLE's host privacy stack keeps the resolving list current.
// On each matching adv we update last_seen_us; a 1Hz tick fires OUT after
// PRESENCE_TIMEOUT_MS without sightings.
//
// iPhone advertising goes very sparse when the screen is locked + no
// nearby Apple devices to keep Find My active — we can easily go 60s+
// without a single visible packet. To avoid false OUTs, the tick fires an
// active connect-probe at PRESENCE_PROBE_AFTER_MS: we ble_gap_connect to
// the peer's identity address, and if iOS accepts (which it will if the
// bond is still resolvable on its side) we refresh last_seen and
// immediately disconnect. If the connect attempt fails or times out, the
// timer eventually crosses PRESENCE_TIMEOUT_MS and OUT fires normally.
#define PRESENCE_MAX           BOND_MAX_LIST
// 60s default but mutable at runtime via dd_ble_set_presence_timeout_s
// (NVS-backed). The probe machinery is sized assuming the default; if you
// turn the timeout way up, probes still fire every PROBE_COOLDOWN_MS.
#define PRESENCE_TIMEOUT_DEFAULT_S 60
#define PRESENCE_TIMEOUT_MIN_S     30
#define PRESENCE_TIMEOUT_MAX_S     600
// Probe schedule for catching iPhones before OUT. iOS BLE peripherals run
// connection intervals of 1-2.5s when locked + idle, so the full connect
// handshake can take 5-7s. PROBE_TIMEOUT used to be 3s — which is *less*
// than one full handshake worst-case. 10s gives the controller two full
// connection intervals to wake + finish handshake even from deep sleep.
//   probe at 15s stale (PROBE_AFTER) → 10s timeout
//   if it failed, retry at ~35s (cooldown 10s after the previous probe ended)
//   OUT at 60s if both failed
#define PRESENCE_PROBE_AFTER_MS   15000
#define PRESENCE_PROBE_TIMEOUT_MS 10000
#define PRESENCE_PROBE_COOLDOWN_MS 10000
// While present=false, keep probing periodically. iPhones go silent on BLE
// adv when locked + idle, so passive scan can miss returning peers
// indefinitely; an active probe reliably wakes them. 30s gives a worst-case
// 30s recovery latency once the phone returns. Bonded peers get pre-
// allocated slots at boot, so this also drives FIRST detection on a fresh
// boot when the iPhone hasn't sent a single adv yet.
#define PRESENCE_REPROBE_OUT_MS  30000
#define PRESENCE_TICK_MS       1000

#define PRESENCE_NVS_NS    "presence"
#define PRESENCE_NVS_KEY   "timeout_s"

static int s_presence_timeout_ms = PRESENCE_TIMEOUT_DEFAULT_S * 1000;

typedef struct {
    uint8_t  addr[6];
    int64_t  last_seen_us;
    int64_t  last_probe_us;     // when last probe attempt started
    bool     present;           // currently in proximity (last_seen recent)
    bool     ack_event;         // IN event has been recorded for this proximity window
    bool     probing;           // a connect-probe is in flight
    bool     connected;         // we hold a live connection to this peer (probe-initiated or HID-accepted)
    uint16_t probe_handle;      // conn handle for in-flight probe (0xFFFF if none)
    bool     used;
} presence_slot_t;

static presence_slot_t s_presence[PRESENCE_MAX];
static esp_timer_handle_t s_presence_tick_timer = NULL;

// Pairing window
static int64_t  s_pairing_until_us = 0;
static esp_timer_handle_t s_pairing_expire_timer = NULL;

// Numeric Comparison state (set by SM PASSKEY_ACTION, cleared by confirm/cancel)
static uint32_t s_pairing_numcmp        = 0;       // 0 = nothing pending
static uint16_t s_pairing_numcmp_handle = 0xFFFF;  // conn awaiting Web UI confirm

// Filter policy: when true, only whitelisted (bonded) peers can connect; when
// false, anyone can connect (used during pairing window or when no bonds exist).
static bool     s_filter_strict = false;

static int gap_event_cb(struct ble_gap_event *event, void *arg);

// ---------- presence: scan-based bonded-peer detection ----------

static void presence_seen_addr(const uint8_t addr[6])
{
    int free_idx = -1;
    int slot_idx = -1;
    for (int i = 0; i < PRESENCE_MAX; i++) {
        if (!s_presence[i].used) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (memcmp(s_presence[i].addr, addr, 6) == 0) { slot_idx = i; break; }
    }
    if (slot_idx < 0) {
        // First sighting of this peer — claim a slot iff it's actually bonded.
        // Otherwise we'd waste slots on neighbours' beacons.
        if (free_idx < 0) {
            ESP_LOGW(TAG, "presence slots full");
            return;
        }
        if (!dd_ble_bond_exists(addr)) return;
        slot_idx = free_idx;
        s_presence[slot_idx].used = true;
        memcpy(s_presence[slot_idx].addr, addr, 6);
        s_presence[slot_idx].present = false;
        s_presence[slot_idx].ack_event = false;
        s_presence[slot_idx].probing = false;
        s_presence[slot_idx].probe_handle = 0xFFFF;
        s_presence[slot_idx].last_probe_us = 0;
    }

    s_presence[slot_idx].last_seen_us = esp_timer_get_time();
    bool first_seen = !s_presence[slot_idx].present;
    s_presence[slot_idx].present = true;

    // Only emit IN once we have wall-clock time. Otherwise the event would
    // land in storage with ts=0 and disappear into 1970 in the UI. The tick
    // backfills as soon as NTP becomes available.
    if (!s_presence[slot_idx].ack_event && dd_time_now_unix() > 0) {
        if (first_seen) {
            ESP_LOGI(TAG, "presence IN %02x:%02x:%02x:%02x:%02x:%02x",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        }
        dd_event_record(DD_EV_IN, DD_SRC_BLE_AUTO, addr, 0, NULL);
        s_presence[slot_idx].ack_event = true;
    }
}

// ---------- presence: active connect-probe ----------
//
// When a bonded peer goes silent (iPhone locked, Find My quiet), we try
// to connect to it ourselves before declaring OUT. iOS accepts the
// connection on a known bond without user prompting, so success/fail is
// a clean "is this device still around" signal. We disconnect immediately
// — we never had any reason to stay connected.

// Forward decls — probe stops/restarts the scanner around its connect.
static void start_scanning(void);
static void schedule_scan_restart(int delay_ms);

// Relax connection params on a held geofence link.
//
// NimBLE defaults give us BLE_GAP_INITIAL_SUPERVISION_TIMEOUT = 0x100 = 2.56s,
// which iOS routinely exceeds when locked + idle (it can go silent on a held
// L2CAP link for 5-10s easily). The result was: probe succeeded → connection
// up → iOS slept past supervision → controller dropped → DISCONNECT → OUT
// timer ran → false OUT after 60s of failed wake-probes, even though the
// iPhone never left the room.
//
// Long supervision (32s = max per spec) + slave latency (peer skips intervals)
// + slow conn interval (1s) keeps the link alive across iOS deep sleep at
// negligible power cost on both sides. iOS may counter-propose tighter values
// — we log the result via BLE_GAP_EVENT_CONN_UPDATE.
static void relax_conn_params(uint16_t conn_handle)
{
    struct ble_gap_upd_params p = {
        .itvl_min            = 800,    //  1000 ms (1.25 ms units)
        .itvl_max            = 1600,   //  2000 ms
        .latency             = 4,      //  peer may skip 4 intervals
        .supervision_timeout = 3200,   // 32000 ms (10 ms units, spec max)
        .min_ce_len          = 0,
        .max_ce_len          = 0,
    };
    int rc = ble_gap_update_params(conn_handle, &p);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "relax_conn_params handle=%d rc=%d", conn_handle, rc);
    }
}

static int probe_event_cb(struct ble_gap_event *event, void *arg)
{
    intptr_t slot_idx = (intptr_t)arg;
    if (slot_idx < 0 || slot_idx >= PRESENCE_MAX) return 0;
    presence_slot_t *s = &s_presence[slot_idx];

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "probe OK slot=%d → holding conn (geofence)", (int)slot_idx);
            s->probe_handle = event->connect.conn_handle;
            s->connected = true;
            s->probing = false;
            // Hold the connection. iOS keeps the BLE link alive while the
            // peer is in range; if the peer leaves range or iOS aggressively
            // drops the link (deep sleep), we'll see DISCONNECT below and
            // start the OUT timer. Routing through presence_seen_addr
            // flips present=true and fires IN if not already.
            presence_seen_addr(s->addr);
            // Slow + tolerant conn params so iOS deep sleep doesn't drop us
            // (default 2.56s supervision is way too tight).
            relax_conn_params(event->connect.conn_handle);
            // Restart scanner — we want to keep seeing other peers' adverts
            // while this connection sits idle.
            schedule_scan_restart(50);
        } else {
            ESP_LOGI(TAG, "probe FAIL slot=%d status=%d", (int)slot_idx, event->connect.status);
            s->probing = false;
            schedule_scan_restart(50);  // probe over, scanner can resume
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (s->probe_handle == event->disconnect.conn.conn_handle) {
            ESP_LOGI(TAG, "geofence link DOWN slot=%d (peer out of range or iOS dropped) reason=0x%02x",
                     (int)slot_idx, event->disconnect.reason);
            s->probe_handle = 0xFFFF;
            s->probing = false;
            s->connected = false;
            // Fresh last_seen so the OUT timer starts NOW. If the peer is
            // still in range, the next OUT-state re-probe (30s) should
            // re-establish quickly. If it's truly gone, OUT fires after
            // PRESENCE_TIMEOUT_MS.
            s->last_seen_us = esp_timer_get_time();
            schedule_scan_restart(50);
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
                ESP_LOGI(TAG, "probe-conn updated handle=%d itvl=%d latency=%d timeout=%d",
                         event->conn_update.conn_handle, d.conn_itvl,
                         d.conn_latency, d.supervision_timeout);
            }
        } else {
            ESP_LOGW(TAG, "probe-conn update failed handle=%d status=%d",
                     event->conn_update.conn_handle, event->conn_update.status);
        }
        return 0;

    default:
        return 0;
    }
}

static void presence_probe_start(int slot_idx)
{
    presence_slot_t *s = &s_presence[slot_idx];
    if (s->probing) return;
    if (dd_ble_pairing_active()) return;  // don't fight the pairing flow

    ble_addr_t peer = { .type = BLE_ADDR_RANDOM_ID };
    memcpy(peer.val, s->addr, 6);

    // Bonds are stored with the type NimBLE captured during pairing —
    // for an iPhone bond that's typically random-static (0x01), not
    // public. Look it up so ble_gap_connect picks the right addr type.
    ble_addr_t peers[BOND_MAX_LIST];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, BOND_MAX_LIST) == 0) {
        for (int i = 0; i < n; i++) {
            if (memcmp(peers[i].val, s->addr, 6) == 0) {
                peer = peers[i];
                break;
            }
        }
    }

    // CRITICAL: ble_gap_connect with peer.type = BLE_ADDR_PUBLIC (0) or
    // BLE_ADDR_RANDOM (1) tells the controller "match this exact address
    // on air" — no resolving-list lookup. iPhones never advertise their
    // identity address; they always rotate through RPAs encrypted with
    // their IRK. So a connect targeting the bare identity address times
    // out 100% of the time (status=13 / BLE_HS_ETIMEOUT in our logs).
    //
    // The controller engages the resolving list (matches RPAs against
    // bonded IRKs back to the identity) only for the _ID variants. Promote
    // PUBLIC → PUBLIC_ID, RANDOM → RANDOM_ID so the probe can actually
    // reach a locked iPhone whose RPA has rotated since pairing.
    if (peer.type == BLE_ADDR_PUBLIC)      peer.type = BLE_ADDR_PUBLIC_ID;
    else if (peer.type == BLE_ADDR_RANDOM) peer.type = BLE_ADDR_RANDOM_ID;

    // ble_gap_connect cannot run while ble_gap_ext_disc is active —
    // they share the controller's scanning resource. Stop the scanner
    // first; it'll be restarted from probe_event_cb's CONNECT-fail or
    // DISCONNECT path (whichever ends the probe). Without this stop the
    // connect call returns BLE_HS_EBUSY (rc=15) and the probe never
    // actually attempts anything.
    int dc = ble_gap_disc_cancel();
    if (dc != 0 && dc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "probe slot=%d disc_cancel rc=%d", slot_idx, dc);
    }

    s->probing = true;
    s->probe_handle = 0xFFFF;
    s->last_probe_us = esp_timer_get_time();

    int rc = ble_gap_connect(s_own_addr_type, &peer,
                             PRESENCE_PROBE_TIMEOUT_MS, NULL,
                             probe_event_cb, (void *)(intptr_t)slot_idx);
    ESP_LOGI(TAG, "probe slot=%d ble_gap_connect rc=%d (peer type=%d)",
             slot_idx, rc, peer.type);
    if (rc != 0) {
        s->probing = false;
        schedule_scan_restart(50);  // restore scanner since probe didn't take
    }
}

static void presence_tick_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < PRESENCE_MAX; i++) {
        if (!s_presence[i].used) continue;

        // Geofence: if we hold a live BLE connection to this peer, presence
        // is definitive. Refresh last_seen continuously so when the link
        // eventually drops (iOS deep-sleep idle, peer leaves range), the
        // OUT timer starts from "now" rather than "minutes ago".
        if (s_presence[i].connected) {
            s_presence[i].last_seen_us = now;
            if (!s_presence[i].present) {
                s_presence[i].present = true;
                s_presence[i].ack_event = false;  // catch-up below fires IN
            }
        }

        // Catch-up: peer is in proximity but we couldn't record IN earlier
        // (NTP wasn't synced yet, or events were just wiped).
        if (s_presence[i].present && !s_presence[i].ack_event &&
            dd_time_now_unix() > 0) {
            ESP_LOGI(TAG, "presence IN %02x:%02x:%02x:%02x:%02x:%02x (backfill)",
                     s_presence[i].addr[0], s_presence[i].addr[1],
                     s_presence[i].addr[2], s_presence[i].addr[3],
                     s_presence[i].addr[4], s_presence[i].addr[5]);
            dd_event_record(DD_EV_IN, DD_SRC_BLE_AUTO, s_presence[i].addr, 0, NULL);
            s_presence[i].ack_event = true;
        }

        // While connected: nothing more to do. No probe scheduling, no OUT
        // timer evaluation. The DISCONNECT handler will drop us into the
        // probe-and-OUT path when the link goes down.
        if (s_presence[i].connected) continue;

        // OUT-state re-probe: keep poking the bonded peer every
        // PRESENCE_REPROBE_OUT_MS to catch silent returns. Without this,
        // a phone that goes through a present→absent→present cycle while
        // locked + idle would never come back to present (passive scan
        // alone misses it). On probe success, presence_seen_addr flips
        // present=true and the catch-up code above fires IN next tick.
        // No OUT event is emitted from this branch — present was already
        // false, so there's nothing to "leave" from.
        if (!s_presence[i].present) {
            if (s_presence[i].probing) continue;
            if (dd_ble_pairing_active()) continue;
            int64_t since_probe = (now - s_presence[i].last_probe_us) / 1000;
            if (s_presence[i].last_probe_us != 0 &&
                since_probe < PRESENCE_REPROBE_OUT_MS) continue;
            ESP_LOGI(TAG, "presence re-probe (OUT-state) %02x:%02x:%02x:%02x:%02x:%02x",
                     s_presence[i].addr[0], s_presence[i].addr[1],
                     s_presence[i].addr[2], s_presence[i].addr[3],
                     s_presence[i].addr[4], s_presence[i].addr[5]);
            presence_probe_start(i);
            continue;
        }
        int64_t since_ms = (now - s_presence[i].last_seen_us) / 1000;

        // Active probe before falling off the cliff. Fire when stale crosses
        // PRESENCE_PROBE_AFTER_MS, with a cooldown so we don't hammer iPhone
        // every tick during the probe-then-out window.
        if (since_ms > PRESENCE_PROBE_AFTER_MS &&
            since_ms <= s_presence_timeout_ms &&
            !s_presence[i].probing) {
            int64_t since_probe = (now - s_presence[i].last_probe_us) / 1000;
            if (s_presence[i].last_probe_us == 0 ||
                since_probe >= PRESENCE_PROBE_COOLDOWN_MS) {
                ESP_LOGI(TAG, "presence stale %llds → probing %02x:%02x:%02x:%02x:%02x:%02x",
                         (long long)(since_ms / 1000),
                         s_presence[i].addr[0], s_presence[i].addr[1],
                         s_presence[i].addr[2], s_presence[i].addr[3],
                         s_presence[i].addr[4], s_presence[i].addr[5]);
                presence_probe_start(i);
            }
        }

        if (since_ms > s_presence_timeout_ms && !s_presence[i].probing) {
            s_presence[i].present = false;
            ESP_LOGI(TAG, "presence OUT %02x:%02x:%02x:%02x:%02x:%02x (no adv for %llds)",
                     s_presence[i].addr[0], s_presence[i].addr[1],
                     s_presence[i].addr[2], s_presence[i].addr[3],
                     s_presence[i].addr[4], s_presence[i].addr[5],
                     (long long)(since_ms / 1000));
            // Only emit OUT if we previously recorded IN (no orphan OUTs).
            if (s_presence[i].ack_event && dd_time_now_unix() > 0) {
                dd_event_record(DD_EV_OUT, DD_SRC_BLE_AUTO,
                                s_presence[i].addr, 0, NULL);
            }
            s_presence[i].ack_event = false;
        }
    }
}

static int disc_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        presence_seen_addr(event->disc.addr.val);
    } else if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        presence_seen_addr(event->ext_disc.addr.val);
    }
    return 0;
}

void dd_ble_presence_reset_events(void)
{
    int n = 0;
    for (int i = 0; i < PRESENCE_MAX; i++) {
        if (s_presence[i].used && s_presence[i].present) {
            s_presence[i].ack_event = false;
            n++;
        }
    }
    ESP_LOGI(TAG, "presence reset: %d in-proximity peers will re-emit IN", n);
}

int dd_ble_get_presence_timeout_s(void)
{
    return s_presence_timeout_ms / 1000;
}

esp_err_t dd_ble_set_presence_timeout_s(int seconds)
{
    if (seconds < PRESENCE_TIMEOUT_MIN_S) seconds = PRESENCE_TIMEOUT_MIN_S;
    if (seconds > PRESENCE_TIMEOUT_MAX_S) seconds = PRESENCE_TIMEOUT_MAX_S;
    s_presence_timeout_ms = seconds * 1000;

    nvs_handle_t h;
    esp_err_t err = nvs_open(PRESENCE_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_i32(h, PRESENCE_NVS_KEY, seconds);
        err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "presence timeout = %ds", seconds);
    return err;
}

static void load_presence_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(PRESENCE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, PRESENCE_NVS_KEY, &v) == ESP_OK &&
        v >= PRESENCE_TIMEOUT_MIN_S && v <= PRESENCE_TIMEOUT_MAX_S) {
        s_presence_timeout_ms = v * 1000;
    }
    nvs_close(h);
}

static void start_scanning(void)
{
    // Forever extended scan, passive (no SCAN_REQ), uncoded 1M PHY only.
    // duplicates filter OFF — we want every adv to refresh last_seen_us.
    struct ble_gap_ext_disc_params uncoded = {
        .itvl   = 0x50,   // 50 ms (in 0.625 ms units)
        .window = 0x30,   // 30 ms
        .passive = 1,
    };
    int rc = ble_gap_ext_disc(s_own_addr_type, 0, 0,
                              0,                          // dups filter off
                              BLE_HCI_SCAN_FILT_NO_WL,
                              0,
                              &uncoded, NULL,
                              disc_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_ext_disc rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "scanner started (50/30 ms passive)");
    }

    if (s_presence_tick_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &presence_tick_cb,
            .name     = "presence_tick",
        };
        if (esp_timer_create(&args, &s_presence_tick_timer) == ESP_OK) {
            esp_timer_start_periodic(s_presence_tick_timer,
                                     (uint64_t)PRESENCE_TICK_MS * 1000);
        }
    }
}

// Defer-restart for the scanner. The connect-probe stops scanning to free
// the controller's scan resource, then needs to restart it from inside a
// GAP CONNECT/DISCONNECT callback — same EBUSY-race danger as adv restart,
// so funnel through esp_timer.
static void scan_restart_cb(void *arg)
{
    start_scanning();
}
static void schedule_scan_restart(int delay_ms)
{
    if (s_scan_restart_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &scan_restart_cb,
            .name     = "scan_restart",
        };
        if (esp_timer_create(&args, &s_scan_restart_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_scan_restart_timer);
    esp_timer_start_once(s_scan_restart_timer, (uint64_t)delay_ms * 1000);
}

// Defer-restart helper. Calling start_advertising synchronously inside the
// GAP event callback (DISCONNECT, ADV_COMPLETE) often races with controller
// cleanup and fails with EBUSY / CMD_DISALLOWED. Schedule a short-delayed
// call from the esp_timer task instead.
static void start_advertising(void);
static void adv_restart_cb(void *arg)
{
    s_advertising = false;
    start_advertising();
}
static void schedule_adv_restart(int delay_ms)
{
    if (s_adv_restart_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &adv_restart_cb,
            .name     = "adv_restart",
        };
        if (esp_timer_create(&args, &s_adv_restart_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_adv_restart_timer);
    esp_timer_start_once(s_adv_restart_timer, (uint64_t)delay_ms * 1000);
}

// ---------- HID descriptor (boot keyboard) ----------
//
// Standard 8-byte boot keyboard report layout:
//   [0] modifier byte (Ctrl/Shift/Alt/GUI bits)
//   [1] reserved
//   [2..7] up to 6 simultaneously-pressed key codes
// We never actually send keystrokes — this descriptor only exists so iOS
// recognizes the device as a keyboard accessory and bonds + auto-reconnects.
static const uint8_t s_kbd_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs) — modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const) — reserved
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x05,        //   Usage Maximum (5)
    0x91, 0x02,        //   Output (Data,Var,Abs) — LED status
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const) — LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Array) — key array
    0xC0,              // End Collection
};

static int register_hid_service(void)
{
    struct ble_svc_hid_params hp = { 0 };

    // bcdHID 0x0111, country 0, flags = remote_wake | normally_connectable
    uint8_t hid_info[4] = { 0x11, 0x01, 0x00, 0x03 };
    memcpy(&hp.hid_info, hid_info, sizeof(hid_info));

    memcpy(hp.report_map, s_kbd_report_map, sizeof(s_kbd_report_map));
    hp.report_map_len  = sizeof(s_kbd_report_map);
    hp.external_rpt_ref = 0;

    hp.proto_mode_present = 1;
    hp.proto_mode = BLE_SVC_HID_PROTO_MODE_REPORT;

    hp.kbd_inp_present = 1;
    hp.kbd_out_present = 1;

    hp.rpts_len = 0;  // boot mode keyboard only — no extra report-mode reports

    int rc = ble_svc_hid_add(hp);
    if (rc != 0) {
        ESP_LOGI(TAG, "ble_svc_hid_add rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "HID service registered (boot keyboard)");
    }
    return rc;
}

// ---------- advertising (extended adv API, instance 0 = HID, legacy PDU) ----------

#define HID_ADV_INSTANCE 0

static void refresh_whitelist(void)
{
    ble_addr_t peers[BOND_MAX_LIST];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, BOND_MAX_LIST) != 0) return;
    if (n > 0) {
        int rc = ble_gap_wl_set(peers, n);
        if (rc != 0) {
            ESP_LOGW(TAG, "wl_set rc=%d", rc);
        } else {
            ESP_LOGI(TAG, "whitelist updated: %d bonded peer(s)", n);
        }
    }
}

static int count_bonds(void)
{
    ble_addr_t peers[BOND_MAX_LIST];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, BOND_MAX_LIST) != 0) return 0;
    return n;
}

// Pre-allocate a presence slot for every bonded peer. Without this, the slot
// is only created when scan first sees an adv from that peer — but iPhones
// locked + idle can stay BLE-silent for minutes (Find My can throttle to <1
// packet per 30s), so on a fresh boot we'd never enter the probe loop and
// would never detect the phone at all. Pre-allocating means the tick will
// see slots in present=false state and run the OUT-state re-probe, which
// reliably wakes locked iPhones via active connect.
static void presence_preallocate_bonded(void)
{
    ble_addr_t peers[BOND_MAX_LIST];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, BOND_MAX_LIST) != 0) return;
    if (n == 0) return;

    // Bulk-lookup latest event per bond in a SINGLE pass over events.jsonl
    // (vs N passes if we did per-peer queries). At 90-day retention this can
    // save several hundred ms of boot time.
    dd_event_peer_latest_t latest[BOND_MAX_LIST] = {0};
    for (int i = 0; i < n; i++) memcpy(latest[i].addr, peers[i].val, 6);
    dd_event_latest_for_peers(latest, n);

    for (int i = 0; i < n && i < PRESENCE_MAX; i++) {
        // Skip if a slot already holds this addr (re-init or duplicate bond).
        bool already = false;
        for (int j = 0; j < PRESENCE_MAX; j++) {
            if (s_presence[j].used &&
                memcmp(s_presence[j].addr, peers[i].val, 6) == 0) {
                already = true;
                break;
            }
        }
        if (already) continue;

        // Find first free slot.
        int free_idx = -1;
        for (int j = 0; j < PRESENCE_MAX; j++) {
            if (!s_presence[j].used) { free_idx = j; break; }
        }
        if (free_idx < 0) {
            ESP_LOGW(TAG, "presence slots full while pre-allocating bonds");
            break;
        }
        s_presence[free_idx].used = true;
        memcpy(s_presence[free_idx].addr, peers[i].val, 6);
        s_presence[free_idx].present = false;
        s_presence[free_idx].ack_event = false;
        s_presence[free_idx].probing = false;
        s_presence[free_idx].connected = false;
        s_presence[free_idx].probe_handle = 0xFFFF;
        s_presence[free_idx].last_probe_us = 0;
        s_presence[free_idx].last_seen_us = 0;

        // Boot-resume: if events log says peer was IN at last shutdown (no
        // matching OUT since), restore the slot's in-RAM state to "still in,
        // already acknowledged". This way:
        //   - peer still in proximity → next scan refreshes last_seen, NO new
        //     event fires (continuous presence, no IN spam, no OUT spam)
        //   - peer actually gone → no scan/probe success, last_seen ages →
        //     after PRESENCE_TIMEOUT_MS the OUT path fires a real OUT (since
        //     ack_event=true), correctly closing the previous IN
        // last_seen primed to NOW so the OUT timer counts from boot, not from
        // 1970, giving the upcoming probe a fair chance.
        bool was_in = latest[i].has_event && latest[i].type == DD_EV_IN;
        if (was_in) {
            s_presence[free_idx].present      = true;
            s_presence[free_idx].ack_event    = true;
            s_presence[free_idx].last_seen_us = esp_timer_get_time();
        }
        ESP_LOGI(TAG, "presence slot %d pre-allocated for bond %02x:%02x:%02x:%02x:%02x:%02x"
                      " (last_event=%s%s)",
                 free_idx,
                 peers[i].val[0], peers[i].val[1], peers[i].val[2],
                 peers[i].val[3], peers[i].val[4], peers[i].val[5],
                 was_in ? "in" : "out/none",
                 was_in ? " — resumed as present" : "");
    }
}

static bool should_be_strict(void)
{
    // Strict (whitelist-only) when not in pairing window AND have bonds.
    if (dd_ble_pairing_active()) return false;
    return count_bonds() > 0;
}

static void start_advertising(void)
{
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID16);

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = APPEARANCE_HID_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.uuids16 = (ble_uuid16_t *)&hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;

    uint8_t buf[31];
    uint8_t buf_sz;
    int rc = ble_hs_adv_set_fields(&fields, buf, &buf_sz, sizeof(buf));
    if (rc != 0) {
        ESP_LOGI(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    s_filter_strict = should_be_strict();
    if (s_filter_strict) refresh_whitelist();

    struct ble_gap_ext_adv_params params = { 0 };
    params.connectable    = 1;
    params.scannable      = 1;   // legacy_pdu IND requires scannable too
    params.legacy_pdu     = 1;
    params.own_addr_type  = s_own_addr_type;
    params.primary_phy    = BLE_HCI_LE_PHY_1M;
    params.secondary_phy  = BLE_HCI_LE_PHY_1M;
    params.tx_power       = 127;
    params.sid            = 0;
    params.filter_policy  = s_filter_strict ? BLE_HCI_ADV_FILT_BOTH
                                            : BLE_HCI_ADV_FILT_NONE;

    // Stop if already configured (e.g. after disconnect or filter change)
    ble_gap_ext_adv_remove(HID_ADV_INSTANCE);

    rc = ble_gap_ext_adv_configure(HID_ADV_INSTANCE, &params, NULL,
                                   gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGI(TAG, "ext_adv_configure rc=%d", rc);
        return;
    }

    struct os_mbuf *data = os_msys_get_pkthdr(buf_sz, 0);
    if (!data) {
        ESP_LOGI(TAG, "os_msys_get_pkthdr failed");
        return;
    }
    rc = os_mbuf_append(data, buf, buf_sz);
    if (rc != 0) {
        os_mbuf_free_chain(data);
        ESP_LOGI(TAG, "os_mbuf_append rc=%d", rc);
        return;
    }
    rc = ble_gap_ext_adv_set_data(HID_ADV_INSTANCE, data);
    if (rc != 0) {
        ESP_LOGI(TAG, "ext_adv_set_data rc=%d", rc);
        return;
    }

    rc = ble_gap_ext_adv_start(HID_ADV_INSTANCE, 0, 0);
    if (rc != 0) {
        ESP_LOGI(TAG, "ext_adv_start rc=%d", rc);
        return;
    }
    s_advertising = true;
    ESP_LOGI(TAG, "advertising '%s' (HID, %s)", s_device_name,
           s_filter_strict ? "whitelist only" : "open");
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d handle=%d",
                 event->connect.status, event->connect.conn_handle);
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                // Push security during pairing window so iPhone gets the
                // numeric-comparison prompt. IN/OUT are NOT recorded here —
                // the scanner-driven presence machine owns attendance now,
                // and a brief HID auto-reconnect from a peer that's been in
                // range for hours shouldn't generate spurious events.
                if (dd_ble_pairing_active()) {
                    ESP_LOGI(TAG, "pairing window open → initiating security");
                    int sec_rc = ble_gap_security_initiate(event->connect.conn_handle);
                    if (sec_rc != 0) {
                        ESP_LOGW(TAG, "security_initiate rc=%d", sec_rc);
                    }
                }
                // iOS auto-reconnects to known HID peripherals roughly
                // every few minutes when the screen is off. That CONNECT
                // is a stronger "still here" signal than scanner adv —
                // route it through presence_seen_addr so:
                //  • IN-state slots get last_seen refreshed (no spurious OUT)
                //  • OUT-state slots flip back to present=true and fire IN
                //    next tick (catch-up code in presence_tick_cb)
                // We deliberately use peer_id_addr (the resolved identity),
                // not the on-air RPA, so it matches the bond store entries.
                presence_seen_addr(desc.peer_id_addr.val);
                // Mark the bonded slot as connected — geofence model. While
                // connected we treat the peer as definitely present, no
                // need to probe / no OUT timer.
                for (int i = 0; i < PRESENCE_MAX; i++) {
                    if (s_presence[i].used &&
                        memcmp(s_presence[i].addr, desc.peer_id_addr.val, 6) == 0) {
                        s_presence[i].connected = true;
                        ESP_LOGI(TAG, "geofence: bonded peer slot %d connected (HID-side)", i);
                        // Same supervision/conn-itvl relaxation as the probe
                        // path: iOS' default for HID is ~6s supervision which
                        // is still tight enough that locked-iPhone deep sleep
                        // can drop the link. Asking for 32s here costs nothing
                        // — iOS may counter-propose, but our request strictly
                        // widens the safe envelope.
                        relax_conn_params(event->connect.conn_handle);
                        break;
                    }
                }
            }
        } else {
            // failed; restart adv (deferred — controller still busy with the
            // failed connection cleanup right now)
            schedule_adv_restart(100);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "disconnect handle=%d reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        // Geofence: clear connected flag for whichever bonded slot owned this
        // conn handle. Refresh last_seen so the OUT timer starts NOW; the
        // OUT-state re-probe will try to re-establish within 30s and only
        // fire OUT after PRESENCE_TIMEOUT_MS of consecutive failures.
        for (int i = 0; i < PRESENCE_MAX; i++) {
            if (s_presence[i].used &&
                memcmp(s_presence[i].addr, event->disconnect.conn.peer_id_addr.val, 6) == 0) {
                s_presence[i].connected = false;
                s_presence[i].last_seen_us = esp_timer_get_time();
                ESP_LOGI(TAG, "geofence: bonded peer slot %d disconnected", i);
                break;
            }
        }
        // Defer adv restart so the controller has time to settle (otherwise
        // wl_set / ext_adv_configure return EBUSY and we end up not
        // advertising at all → iPhone can't auto-reconnect).
        schedule_adv_restart(100);
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete reason=%d", event->adv_complete.reason);
        schedule_adv_restart(50);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
                ESP_LOGI(TAG, "HID-conn updated handle=%d itvl=%d latency=%d timeout=%d",
                         event->conn_update.conn_handle, d.conn_itvl,
                         d.conn_latency, d.supervision_timeout);
            }
        } else {
            ESP_LOGW(TAG, "HID-conn update failed handle=%d status=%d",
                     event->conn_update.conn_handle, event->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe handle=%d attr=%d notify=%d indicate=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify, event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        // With sm_io_cap = KEYBOARD_DISPLAY and an iOS Central, SM picks
        // Numeric Comparison: SM derives a 6-digit number from the SC
        // public-key exchange and asks BOTH sides to confirm the number
        // matches. The actual security comes from the human comparing the
        // two numbers and tapping "Pair" on the iPhone — that's the leg
        // an attacker can't forge. Our side's accept is essentially
        // mechanical, so just auto-accept and let the user verify by
        // looking at the number on the Web UI vs what their iPhone shows.
        // The Web UI exposes the numcmp value via /api/pairing/status; if
        // the user spots a mismatch they can tap "Cancel pairing" which
        // hits /api/pairing/cancel and drops the connection.
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            if (!dd_ble_pairing_active()) {
                ESP_LOGW(TAG, "NC requested but pairing window closed — rejecting");
                struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 0 };
                ble_sm_inject_io(event->passkey.conn_handle, &io);
                return 0;
            }
            s_pairing_numcmp        = event->passkey.params.numcmp;
            s_pairing_numcmp_handle = event->passkey.conn_handle;
            ESP_LOGI(TAG, "NC numcmp=%06" PRIu32 " handle=%d (auto-accept)",
                     s_pairing_numcmp, event->passkey.conn_handle);
            struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1 };
            ble_sm_inject_io(event->passkey.conn_handle, &io);
            return 0;
        }
        ESP_LOGW(TAG, "unexpected passkey action=%d, denying",
                 event->passkey.params.action);
        return BLE_HS_EREJECT;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change handle=%d status=%d",
                 event->enc_change.conn_handle, event->enc_change.status);
        if (event->enc_change.status == 0) {
            // Successful pairing closes the window automatically. Pre-allocate
            // a presence slot for this brand-new bond so the OUT-state
            // re-probe loop will start exercising it immediately, instead of
            // waiting for the iPhone to send an unsolicited adv.
            presence_preallocate_bonded();
            dd_ble_pairing_cancel();
            schedule_adv_restart(100);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Peer is trying to encrypt with an LTK we no longer have (typically
        // an iPhone reconnecting after we revoked its bond from the Web UI).
        //
        // NimBLE's *default* behaviour when the gap callback doesn't handle
        // this event is to silently delete any stale store record and accept
        // a fresh pairing — which means a revoked iPhone walks back into
        // range and re-bonds itself with no human consent. So:
        //
        //   pairing window OPEN  → delete stale record, allow fresh pairing
        //   pairing window CLOSED → IGNORE; SM aborts and the conn drops
        //
        // The whitelist filter (refresh_whitelist) takes care of blocking
        // subsequent connection attempts from this peer.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        if (dd_ble_pairing_active()) {
            ESP_LOGI(TAG, "repeat-pairing: window open → RETRY");
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        ESP_LOGW(TAG, "repeat-pairing: window CLOSED → IGNORE (peer must "
                      "re-pair through Web UI)");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset reason=%d", reason);
    s_advertising = false;
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "on_sync entered");
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGI(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGI(TAG, "id_infer_auto rc=%d", rc);
        return;
    }

    uint8_t addr[6];
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "own BLE addr (type=%d): %02x:%02x:%02x:%02x:%02x:%02x",
             s_own_addr_type, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    // Pre-populate presence slots for every existing bond. Without this the
    // tick has nothing to drive probes off of until scan happens to receive
    // an unsolicited adv from the peer — which iPhones locked + idle can
    // skip for minutes. With pre-allocated slots, the OUT-state re-probe
    // below kicks in immediately at boot and reliably detects locked phones.
    presence_preallocate_bonded();

    start_advertising();
    start_scanning();
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t dd_ble_start(void)
{
    // NimBLE logs "GAP procedure initiated..." chatter at INFO. Quiet it.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    load_presence_settings();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_device_name, sizeof(s_device_name), "dingdong-%02X%02X",
             mac[4], mac[5]);
    // Override default with NVS-stored custom name if user set one.
    {
        char custom[DD_DEVICE_NAME_MAX];
        if (dd_config_get_device_name(custom, sizeof(custom)) == ESP_OK &&
            custom[0] != '\0') {
            strncpy(s_device_name, custom, sizeof(s_device_name) - 1);
            s_device_name[sizeof(s_device_name) - 1] = '\0';
            ESP_LOGI(TAG, "using custom device name: %s", s_device_name);
        }
    }

    ESP_LOGI(TAG, "nimble_port_init...");
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nimble_port_init=%s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Security: LE SC + bonding + Display-only (passkey shown by us, entered on iPhone).
    // Actual passkey injection happens in the pairing-window code (M4 part 3).
    ble_hs_cfg.sm_io_cap        = BLE_SM_IO_CAP_KEYBOARD_DISP;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ESP_LOGI(TAG, "ble_svc_gap_init");
    ble_svc_gap_init();
    ESP_LOGI(TAG, "ble_svc_gatt_init");
    ble_svc_gatt_init();
    ESP_LOGI(TAG, "services initialized");

    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) ESP_LOGI(TAG, "set_device_name rc=%d", rc);

    rc = ble_svc_gap_device_appearance_set(APPEARANCE_HID_KEYBOARD);
    if (rc != 0) ESP_LOGI(TAG, "set_appearance rc=%d", rc);

    ESP_LOGI(TAG, "calling register_hid_service");
    rc = register_hid_service();
    if (rc != 0) {
        ESP_LOGE(TAG, "HID register failed rc=%d, continuing without HID", rc);
    } else {
        ESP_LOGI(TAG, "ble_svc_hid_init");
        ble_svc_hid_init();   // must come AFTER all ble_svc_hid_add calls
    }

    ESP_LOGI(TAG, "calling nimble_port_freertos_init");
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "host_task launched");
    return ESP_OK;
}

bool        dd_ble_is_advertising(void) { return s_advertising; }
const char *dd_ble_device_name(void)    { return s_device_name; }

esp_err_t dd_ble_set_device_name(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) >= sizeof(s_device_name))
        return ESP_ERR_INVALID_ARG;

    strncpy(s_device_name, name, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';

    ble_svc_gap_device_name_set(s_device_name);

    // Restart adv to embed new name in the adv data
    s_advertising = false;
    ble_gap_ext_adv_stop(HID_ADV_INSTANCE);
    start_advertising();
    return ESP_OK;
}

// ---------- pairing window ----------

static void clear_numcmp_pending(bool reject_if_pending)
{
    if (s_pairing_numcmp_handle != 0xFFFF && reject_if_pending) {
        struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 0 };
        ble_sm_inject_io(s_pairing_numcmp_handle, &io);
    }
    s_pairing_numcmp = 0;
    s_pairing_numcmp_handle = 0xFFFF;
}

static void on_pairing_expire(void *arg)
{
    if (s_pairing_until_us != 0 && esp_timer_get_time() >= s_pairing_until_us) {
        ESP_LOGI(TAG, "pairing window expired, restoring whitelist filter");
        s_pairing_until_us = 0;
        clear_numcmp_pending(true);
        s_advertising = false;
        ble_gap_ext_adv_stop(HID_ADV_INSTANCE);
        start_advertising();
    }
}

esp_err_t dd_ble_pairing_start(int64_t ttl_ms)
{
    if (ttl_ms < 5000)   ttl_ms = 5000;
    if (ttl_ms > 120000) ttl_ms = 120000;

    s_pairing_until_us = esp_timer_get_time() + ttl_ms * 1000;
    clear_numcmp_pending(false);

    ESP_LOGI(TAG, "pairing window open (NC) ttl=%lldms", (long long)ttl_ms);

    if (s_pairing_expire_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &on_pairing_expire,
            .name = "pair_expire",
        };
        esp_timer_create(&args, &s_pairing_expire_timer);
    }
    esp_timer_stop(s_pairing_expire_timer);
    esp_timer_start_once(s_pairing_expire_timer, (uint64_t)ttl_ms * 1000);

    s_advertising = false;
    ble_gap_ext_adv_stop(HID_ADV_INSTANCE);
    start_advertising();
    return ESP_OK;
}

void dd_ble_pairing_cancel(void)
{
    bool was_active = (s_pairing_until_us != 0);
    if (was_active) ESP_LOGI(TAG, "pairing window closed");
    s_pairing_until_us = 0;
    clear_numcmp_pending(true);
    if (s_pairing_expire_timer) esp_timer_stop(s_pairing_expire_timer);

    if (was_active) {
        // Defer the adv restart instead of doing it synchronously here:
        // pairing_cancel is called from BLE_GAP_EVENT_ENC_CHANGE (post-bond)
        // in addition to HTTP cancel + window-expire timer. Calling
        // start_advertising synchronously from a GAP callback hits the
        // EBUSY race the CLAUDE.md warns about, leaves s_advertising=false
        // and the status badge stuck on "BLE". Defer-everywhere is safer
        // than branching on caller context.
        s_advertising = false;
        ble_gap_ext_adv_stop(HID_ADV_INSTANCE);
        schedule_adv_restart(50);
    }
}

bool dd_ble_pairing_active(void)
{
    return s_pairing_until_us > esp_timer_get_time();
}

uint32_t dd_ble_pairing_numcmp(void)
{
    return s_pairing_numcmp;
}

esp_err_t dd_ble_pairing_confirm(bool accept)
{
    if (s_pairing_numcmp_handle == 0xFFFF) {
        return ESP_ERR_INVALID_STATE;
    }
    struct ble_sm_io io = {
        .action = BLE_SM_IOACT_NUMCMP,
        .numcmp_accept = accept ? 1 : 0,
    };
    int rc = ble_sm_inject_io(s_pairing_numcmp_handle, &io);
    ESP_LOGI(TAG, "NC confirm accept=%d handle=%d rc=%d",
             accept, s_pairing_numcmp_handle, rc);
    s_pairing_numcmp = 0;
    s_pairing_numcmp_handle = 0xFFFF;
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

int64_t dd_ble_pairing_remaining_ms(void)
{
    int64_t now = esp_timer_get_time();
    if (s_pairing_until_us <= now) return 0;
    return (s_pairing_until_us - now) / 1000;
}

// ---------- bond management ----------

int dd_ble_bond_list(uint8_t (*out_addrs)[6], int cap)
{
    if (!out_addrs || cap <= 0) return 0;

    ble_addr_t peers[BOND_MAX_LIST];
    int n_peers = 0;
    int rc = ble_store_util_bonded_peers(peers, &n_peers,
                                         cap < BOND_MAX_LIST ? cap : BOND_MAX_LIST);
    if (rc != 0) {
        ESP_LOGW(TAG, "bonded_peers rc=%d", rc);
        return 0;
    }
    int copied = n_peers < cap ? n_peers : cap;
    for (int i = 0; i < copied; i++) {
        memcpy(out_addrs[i], peers[i].val, 6);
    }
    return copied;
}

esp_err_t dd_ble_bond_revoke(const uint8_t addr[6])
{
    if (!addr) return ESP_ERR_INVALID_ARG;

    // Find the bond by raw bytes — identity address has already been resolved
    // by NimBLE so peers[i].val is what the user sees in the bonds UI.
    ble_addr_t peers[BOND_MAX_LIST];
    int n_peers = 0;
    ble_store_util_bonded_peers(peers, &n_peers, BOND_MAX_LIST);

    ble_addr_t matched = { 0 };
    bool found = false;
    for (int i = 0; i < n_peers; i++) {
        if (memcmp(peers[i].val, addr, 6) == 0) {
            matched = peers[i];
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "bond_revoke: %02x:%02x:%02x:%02x:%02x:%02x not in store (n=%d)",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], n_peers);
        return ESP_ERR_NOT_FOUND;
    }

    // Terminate live connection ourselves. ble_gap_unpair tries to do this
    // too, but returns BLE_HS_EBUSY in certain controller states even when
    // no termination is actually needed — and that aborts the whole revoke.
    for (uint16_t h = 0; h < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; h++) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(h, &desc) != 0) continue;
        if (memcmp(desc.peer_id_addr.val, addr, 6) == 0) {
            ESP_LOGI(TAG, "terminating live conn handle=%d for revoked peer", h);
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
        }
    }

    // Delete bond entries (peer_sec, our_sec, CCCDs) directly. Bypasses the
    // ble_gap_unpair EBUSY trap.
    int rc = ble_store_util_delete_peer(&matched);
    if (rc != 0) {
        ESP_LOGW(TAG, "bond_revoke: store_delete rc=%d for %02x:%02x:%02x:%02x:%02x:%02x",
                 rc, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "bond revoked: %02x:%02x:%02x:%02x:%02x:%02x (type=%d)",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], matched.type);

    // Free the presence slot for this addr — the peer is no longer ours, so
    // we shouldn't keep probing it. Without this the OUT-state re-probe
    // would hammer connection requests at a peer whose IRK we no longer hold.
    for (int i = 0; i < PRESENCE_MAX; i++) {
        if (s_presence[i].used && memcmp(s_presence[i].addr, addr, 6) == 0) {
            memset(&s_presence[i], 0, sizeof(s_presence[i]));
            ESP_LOGI(TAG, "presence slot %d freed (bond revoked)", i);
            break;
        }
    }

    // Restart adv (will drop strict-filter mode if this was the last bond).
    s_advertising = false;
    ble_gap_ext_adv_stop(HID_ADV_INSTANCE);
    start_advertising();
    return ESP_OK;
}

bool dd_ble_bond_exists(const uint8_t addr[6])
{
    if (!addr) return false;
    ble_addr_t peers[BOND_MAX_LIST];
    int n_peers = 0;
    if (ble_store_util_bonded_peers(peers, &n_peers, BOND_MAX_LIST) != 0) return false;
    for (int i = 0; i < n_peers; i++) {
        if (memcmp(peers[i].val, addr, 6) == 0) return true;
    }
    return false;
}

int dd_ble_presence_snapshot(dd_ble_presence_snapshot_t *out, int cap)
{
    if (!out || cap <= 0) return 0;
    int64_t now = esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < PRESENCE_MAX && n < cap; i++) {
        if (!s_presence[i].used) continue;
        out[n].used      = true;
        memcpy(out[n].addr, s_presence[i].addr, 6);
        out[n].present   = s_presence[i].present;
        out[n].ack_event = s_presence[i].ack_event;
        out[n].connected = s_presence[i].connected;
        out[n].probing   = s_presence[i].probing;
        out[n].last_seen_ago_ms  = s_presence[i].last_seen_us  ? (now - s_presence[i].last_seen_us)  / 1000 : -1;
        out[n].last_probe_ago_ms = s_presence[i].last_probe_us ? (now - s_presence[i].last_probe_us) / 1000 : -1;
        n++;
    }
    return n;
}
