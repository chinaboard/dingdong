# dingdong-fw

ESP32-C6 firmware: a BLE-based attendance tracker with an embedded Web UI.

## How it works

- **Continuous BLE scanning.** The device passively scans for advertisements
  and resolves Resolvable Private Addresses against IRKs collected during
  pairing. When a bonded iPhone shows up → `check_in`; when it's been gone
  for >15s → `check_out`. iOS reconnect cadence does not affect attendance.
- **HID keyboard peripheral.** Advertised only so iOS will agree to bond and
  exchange the IRK; never actually sends keystrokes.
- **Numeric Comparison pairing.** The 6-digit code shows up on the Web UI
  and on the iPhone simultaneously; user verifies on both.
- **Web UI** (admin console + first-run wizard, EN/ZH i18n) drives pairing,
  worker management, calendar / CSV export, OTA, and configuration. Single
  HTML page, gzipped at build time and embedded into the firmware.
- **Captive portal.** In SoftAP setup mode, joining the open WiFi auto-pops
  the setup page (DNS hijack + HTTP catch-all).
- **NVS** holds admin password (PBKDF2-SHA256), WiFi creds, worker
  registry, BLE bonds. **LittleFS** holds the append-only `events.jsonl`.
- **NTP** keeps wall time; pre-sync events backfill from monotonic clock.

## Build & flash

Requires Docker (Desktop / OrbStack) for the build, `espflash` on the host
for flashing (USB passthrough doesn't work inside macOS Docker).

```bash
brew install espflash         # one-time

make build                    # compile in espressif/idf:v6.0.1
make flash                    # write to /dev/cu.usbmodem101 @ 921600
make monitor                  # serial log
make flash-monitor

make menuconfig               # interactive sdkconfig (rare)
make erase                    # full chip erase (needed when partition layout changes)
make fullclean                # nuke build/ + sdkconfig + managed_components
```

Override port: `make flash PORT=/dev/cu.xxx`.

## Releases

Tagging drives releases. `git tag v0.5.6 && git push --tags` triggers
the GitHub Actions workflow which builds and attaches a flashable
`dingdong-merged-<ver>.bin` (single `esptool write_flash 0x0`) plus the
individual partition assets to the GitHub Release.

The version baked into the firmware comes from `git describe --tags
--dirty --always` at build time, so off-tag dev builds get a
self-describing string like `0.5.5-3-gabc1234-dirty`.

## License

MIT — see [LICENSE](./LICENSE). Built on top of ESP-IDF (Apache 2.0); the
firmware binary embeds compiled IDF code under those terms.
