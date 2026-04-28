# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP-IDF firmware for ESP32-C6 (`espressif/idf:v6.0.1`). The device runs **continuous BLE scanning** to detect bonded iPhones in proximity via Resolvable-Private-Address (RPA) resolution against stored IRKs. Each "iPhone in range" → `check_in`, "out of range for >15s" → `check_out`. The device also advertises as a BLE HID keyboard (so iOS will bond and exchange the IRK in the first place). An admin Web UI (HTTP, single embedded gzipped HTML page) drives pairing, worker management, calendar/CSV export, and configuration. A WS2812 status LED on GPIO8 surfaces device health.

Single binary produced: `build/dingdong.elf`. Target is fixed in `sdkconfig.defaults` (do not change in `menuconfig`).

## Build / Flash / Monitor

Build runs **inside Docker** (macOS Docker can't pass USB through); flash/monitor run on the **host** with `espflash`. Both halves are wrapped by `Makefile`:

```bash
make build                          # idf.py build inside espressif/idf:v6.0.1
make flash                          # espflash on host → /dev/cu.usbmodem101
make monitor                        # espflash monitor (interactive)
make flash-monitor
make menuconfig                     # interactive sdkconfig (TTY container)
make erase                          # full chip erase via espflash
make size                           # idf.py size
make shell                          # bash inside the IDF container
make clean                          # idf.py fullclean
make fullclean                      # rm build/ sdkconfig managed_components/ dependencies.lock
```

Override port: `make flash PORT=/dev/cu.xxx` (default `/dev/cu.usbmodem101`, baud `921600`). Headless serial-only sniff (when monitor's TTY requirement is in the way): `python3 /tmp/dd_log.py` (pyserial; opens /dev/cu.usbmodem101 @ 115200 raw).

There is no host toolchain assumption beyond Docker + `espflash` (`brew install espflash`). There is no test suite, no linter, and no CI configuration in-tree.

## Build-time configuration

A few knobs are baked at compile time and propagated through `EXTRA_CFLAGS` from the `Makefile`. Override per-invocation:

```bash
make build TZ=JST-9                   # POSIX TZ string baked into dd_time_init as fallback
make build LED_GPIO=15                # WS2812 pin (default 8 for SuperMini)
make build LED_BRIGHTNESS=1           # 0..255, sets per-channel ceiling for status LED
make build LED_ENABLE=0               # skip LED code entirely (board has no addressable pixel)
```

Each macro has a `#ifndef … #define` fallback in the consuming `.c`, so a bare `idf.py build` (no Makefile) still compiles with sensible defaults. **TZ is build-time only** — it sets the device's `setenv("TZ", ...)` for `localtime_r` calls (CSV export columns, `/api/today` / `/api/calendar` day boundaries). The Web UI displays times in the browser's timezone via `new Date(ts*1000).getHours()`, so the device-side TZ only matters for export and bucketing, not for what the dashboard renders.

## Versioning

Tag-driven via `git describe --tags --dirty --always`, run from `CMakeLists.txt`. Three cases:

- **On a clean tag** (`git tag v0.5.5`): version string is the bare semver, e.g. `0.5.5`.
- **N commits past the last tag**: e.g. `0.5.5-3-gabc1234`.
- **Working tree dirty**: `-dirty` appended.

The `VERSION` file at the repo root is a fallback only (used when there's no git or no tags reachable, e.g. tarball checkout). To cut a release: `git tag v0.5.5 && git push --tags` — CI builds, attaches assets, and the version baked into `esp_app_desc_t.version` (and surfaced in bootloader logs / OTA descriptors / `/api/system/diag` / Web UI header) all match the tag exactly. `esp_app_desc_t.version[32]` caps at 31 chars, enforced by a `string(SUBSTRING)` truncation in `CMakeLists.txt`. Do **not** hardcode versions elsewhere.

## Top-level architecture

`main/main.c` wires everything in this fixed order — later components depend on earlier ones being initialized:

```
dd_config_init      NVS + PSA crypto
dd_session_init     in-RAM admin session table (4 slots, 7-day TTL)
dd_metrics_record_boot
dd_time_init        applies TZ from NVS (fallback DD_TZ macro); SNTP started later
dd_storage_init     LittleFS mount on "storage" partition → /storage
dd_worker_init      load NVS-backed worker registry into RAM
dd_ble_start        NimBLE host: peripheral (HID adv) + observer (continuous scan) + bond store
dd_led_init         WS2812 status LED (no-op when DD_LED_ENABLE=0)
dd_wifi_start       SoftAP or STA (depends on dd_config_boot_mode)
dd_http_start       HTTP server on :80 (serves embedded gzipped web/index.html)
dd_time_sntp_start  background NTP retries until WiFi up
```

After init, `app_main` enters an alive-tick loop (10s) plus three `esp_timer` callbacks set up at the bottom of `app_main`:
- **OTA validate** (one-shot, 60 s): marks the running OTA image valid so the bootloader stops rolling back. If we panic in the first 60 s, the bootloader reverts. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in sdkconfig.defaults makes this load-bearing.
- **Heap watchdog** (every 10 s): warns at <20 KB free, hard-restarts at <6 KB.
- **Retention** (daily + once at 60 s): trims `events.jsonl` older than `DD_EVENTS_RETENTION_DAYS` (90). The 512 KB rotation in `dd_storage` is just a safety cap.

A separate FreeRTOS task watches GPIO9 (BOOT button); a 5-second long-press calls `dd_config_factory_reset()` + `dd_storage_event_wipe()` then `esp_restart()`. This is the only physical recovery path.

## Boot modes

`dd_config_boot_mode()` returns one of three states based purely on what's in NVS, and `dd_wifi_start()` switches its WiFi mode off it:

| Boot mode | Trigger | WiFi mode |
|-----------|---------|-----------|
| `DD_BOOT_FIRST_RUN` | no admin password set | SoftAP `dingdong-setup-XXXX` (open) |
| `DD_BOOT_NO_WIFI`   | admin set, no WiFi creds | SoftAP `dingdong-setup-XXXX` |
| `DD_BOOT_NORMAL`    | both present | STA |

`XXXX` is the last 2 bytes of the WiFi STA MAC, so multiple devices in setup mode stay distinguishable on the same scan list.

In SoftAP modes the Web UI is the first-run wizard; in STA mode it's the admin console. STA mode auto-restarts the device after 10 minutes of continuous disconnection (`STA_GIVE_UP_MS` in `dd_wifi.c`) so a transient AP outage doesn't strand the device.

## Presence detection (the core architecture)

The attendance signal is **not** driven by HID connect/disconnect events — that approach was unreliable because iOS controls reconnection cadence and can take minutes to wake. Instead:

1. **Initial bond**: user opens pairing window → iPhone connects to the HID peripheral → SC pairing exchanges IRK → bond saved in NVS. (Pairing method is **Numeric Comparison** — both sides display the same 6-digit number; user verifies on iPhone + Web UI.)
2. **Steady state**: device runs continuous extended-discovery scan (50 ms interval / 30 ms window, passive). NimBLE auto-resolves incoming RPAs against the resolving list (which is populated from NVS bonds at boot). When the resolved identity address matches a bond, we update `last_seen_us` for that peer.
3. **Tick** (1 Hz): for each bonded peer, if `last_seen_us` is older than 15 s → fire OUT. If we have a sighting but haven't recorded IN yet (because NTP wasn't synced) → fire IN with the now-valid timestamp.
4. **HID connection events** (CONNECT/DISCONNECT) are kept in the GAP callback **only** to push security-initiate during the pairing window — they no longer drive IN/OUT. iPhone may briefly auto-reconnect to the HID service while a bond is active; that's irrelevant to attendance.

This is why iPhone reaction time is ≈1 second on enter and ≤15 seconds on leave, regardless of whether iOS feels like reconnecting.

## Components and their boundaries

Each `components/<name>/` is an isolated IDF component with `include/dd_<name>.h` as its public surface. Inter-component deps are declared in each `CMakeLists.txt`'s `REQUIRES`/`PRIV_REQUIRES`; respect them — do not include `dd_*.h` from a component whose CMake doesn't list the producer in `REQUIRES`.

- `config` — NVS-backed admin password (PBKDF2-SHA256, 16-byte salt, 32-byte hash, 20k iters), WiFi creds, device name, lifetime metrics. `dd_config_factory_reset()` wipes everything.
- `session` — opaque 64-hex-char tokens written into the `dd_session` cookie by `http_app`. RAM-only; lost on reboot.
- `wifi_mgr` — owns the `esp_netif`/`esp_wifi`/`esp_event` setup. Boot-mode aware (see above).
- `ble` — NimBLE host. Roles: **Peripheral** (HID adv on instance 0, legacy_pdu for iPhone backward-compat), **Observer** (continuous scan for the presence machine).
  - **Pairing**: SC + bonding + MITM, IO cap = `KEYBOARD_DISPLAY` → with iOS this gives **Numeric Comparison**. The 6-digit value is captured in `BLE_GAP_EVENT_PASSKEY_ACTION` and exposed via `dd_ble_pairing_numcmp()`; UI polls it via `/api/pairing/status`. On user click, `/api/pairing/confirm` calls `dd_ble_pairing_confirm(true)`. Pairing window required to allow new bonds; outside the window REPEAT_PAIRING is `IGNORE`d (so revoked iPhones with stale LTKs can't silently re-bond).
  - **Bond revoke**: `dd_ble_bond_revoke()` finds the bond by exact address bytes (sidesteps NimBLE's strict address-type matching), terminates any live connection from the same peer, and deletes via `ble_store_util_delete_peer` (avoids `ble_gap_unpair`'s EBUSY trap).
  - **Presence machine**: per-bond slot table with `last_seen_us`, `present`, `ack_event`. `dd_ble_presence_reset_events()` (called by `events_wipe`) clears `ack_event` so the next tick re-emits IN for everyone in proximity.
  - **Note on adv instances**: instance 0 is HID (legacy PDU). The ESP32-C6 BLE 5 controller can only sustain **one legacy advertising set at a time**, so don't add a second legacy adv instance — it'll either fail to start or push HID off the air. Extended PDU is fine for additional sets if needed.
- `time_sync` — `esp_sntp` + monotonic↔wall translation. TZ is set once from the build-time `DD_TZ` macro (default `"CST-8"`); no runtime setter. `dd_time_mono_to_unix(mono_us)` is how callers backfill timestamps for events captured before NTP sync.
- `storage` — LittleFS mount at `/storage`, single append-only `events.jsonl`. All file mutation is mutex-serialized. `dd_storage_event_count()` is cached (updated on append/rotate); don't replace it with a per-call scan. Rotation is two-tier: daily time-based (primary) and 512 KB hard cap (fallback). `dd_storage_event_delete_by_worker(id)` rewrites the file filtering out events for that worker (used by hard-delete worker flow).
- `events` — thin layer above `storage`: builds the JSON line, looks up worker_id by peer addr, and applies a **10 s debounce per (peer, type)** — but only for `DD_SRC_BLE_AUTO`. Manual web/button clicks are intentional and pass through.
- `workers` — NVS-backed worker registry, in-RAM cache of up to 32 entries. **Hard delete** via `dd_worker_delete(id)` — slot is freed and id can be reused. (Old soft-revoke field `dd_worker_t.revoked` is kept for backup-restore compat but no longer set.) `dd_worker_wipe` exists only for `/api/system/restore`.
- `led` — single WS2812 driven via `espressif/led_strip` managed component. 100 ms `esp_timer` polls `dd_wifi_state()` / `dd_time_is_synced()` / `dd_ble_pairing_active()` / heap and pushes a colour: red fast-blink (heap critical) > cyan fast-blink (pairing window) > purple breath (SoftAP) > blue fast-blink (STA connecting/down) > yellow slow-blink (STA up, no NTP) > dim green steady (all good). Runtime on/off via `dd_led_set_enabled()` (persisted in NVS, exposed at `/api/system/led`). Build-time `DD_LED_ENABLE=0` compiles out the whole state machine.
- `http_app` — split across 4 files plus the Web UI:
  - `dd_http.c` (~250 lines) — server lifecycle, shared helpers (`reply_json_status`, `reply_text`, `recv_json_body`, cookie + auth), the route table, and `root_get` which serves `web/index.html` via a **gzipped binary blob** (built at CMake time, embedded with `target_add_binary_data`; saves ~20KB vs raw HTML). Sends `Content-Encoding: gzip`.
  - `dd_http_system.c` — auth, system control, OTA, backup/restore, diag, public health/metrics/status, /api/system/key reveal.
  - `dd_http_attendance.c` — events log + today summary + calendar + paired (in/out → segments) view + CSV/JSONL export. The pair / today / calendar iter helpers are local to this file.
  - `dd_http_devices.c` — workers (incl. hard-delete via `/api/workers/delete`), bonds, pairing window (start/cancel/status/confirm), first-run setup, WiFi scan.
  - `http_internal.h` — shared decls (helpers + every handler signature) so the route table in `dd_http.c` can reference handlers defined in sibling files. **Not exported**; outside consumers still use `dd_http.h`.
  - `web/index.html` — the entire dashboard, Linear/Vercel-style dark UI. Edit as plain HTML/CSS/JS; CMake gzips on build, `make build && make flash` ships the new UI.
  - Auth model: cookie `dd_session` checked by `require_auth()`. `/health` and `/metrics` are public; `/api/setup`, `/api/auth/login`, `/api/wifi/scan`, `/api/system/status`, `/api/pairing/status` skip auth on purpose (first-run / login flow / status badge). OTA upload is `POST /api/system/ota`.

## Partition layout

`partitions.csv` is custom (referenced from `sdkconfig.defaults`). Two 1.5 MB OTA app slots (`ota_0` / `ota_1`) plus an 896 KB `storage` LittleFS partition (mounted at `/storage`, label `"storage"`). NVS is the default 24 KB. Resizing OTA slots requires a coordinated rebuild + flash erase — devices upgrading from a different layout MUST `make erase` first; OTA between incompatible partition tables silently bricks the slot.

## Size budget

Image targets ≈1.37 MB / 91% of the 1.5 MB OTA slot. Key sdkconfig knobs that keep us there:
- `COMPILER_OPTIMIZATION_SIZE=y` (was DEBUG / `-Og` in stock IDF — single biggest win, ~200 KB)
- `NEWLIB_NANO_FORMAT=y` (smaller printf without float by default)
- `LWIP_IPV6=n` (we don't use it on the local network)
- `MBEDTLS_TLS_CLIENT=n` / `MBEDTLS_TLS_SERVER=n` / `ESP_HTTP_CLIENT_ENABLE_HTTPS=n` — no HTTPS anywhere
- `MBEDTLS_SHA1_C=n` / `MBEDTLS_SHA384_C=n` / `MBEDTLS_SHA512_C=n` — only PBKDF2-SHA256 needed
- `BT_NIMBLE_ROLE_CENTRAL=n` — we never initiate connections (Observer is enough for our scanning)

## Conventions worth preserving

- Public headers are wrapped with `extern "C"`, named `dd_<component>.h`, and live in `include/`.
- Each `.c` defines a file-scope `static const char *TAG = "<short>"` for `ESP_LOG*`. Subsystems that flood the log (NimBLE GAP, WiFi/PHY) are quieted to `WARN` in `app_main`.
- The alive-tick loop hashes `(wifi_state, ble_advertising, ntp_synced, event_count)` and only re-logs when that hash changes (or every 6th tick = 60 s). Preserve this pattern when adding state — silent normality matters for serial readability.
- Anything called from both an interrupt/host task and an HTTP handler must be mutex-protected (see `s_lock` in `storage`, `worker`, `session`, `events`).
- New HTTP routes: add the handler in the appropriate `dd_http_<domain>.c`, declare it in `http_internal.h`, and append a row to the `routes[]` table in `dd_http.c`. Don't add files to the root or split a domain further unless one of the existing files crosses ~1000 lines.
- BLE adv restarts from inside GAP callbacks must go through `schedule_adv_restart(ms)` (deferred via `esp_timer`). Calling `start_advertising()` synchronously from CONNECT/DISCONNECT/ADV_COMPLETE handlers races with controller cleanup and gets EBUSY.
- **Commit messages**: subject line is the release-notes line — GitHub auto-generates release notes from commit subjects between tags (workflow uses `generate_release_notes: true`). So write subjects like a changelog entry: imperative verb, concise, scope obvious (`Make SoftAP SSID per-device`, not `update wifi mgr`). Long-form explanation goes in the commit body — visible in `git log` / `gh pr view`, not in the release page.

# CLAUDE.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.
