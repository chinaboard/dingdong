# dingdong-fw

ESP32-C6 firmware: BLE HID-based attendance tracker with Web UI.

## Architecture

- **BLE HID peripheral** — iPhones bond once via Web-UI-driven passkey pairing,
  then auto-reconnect when in range. Each connect/disconnect → `check_in`/`check_out`.
- **BTHome v2 time broadcaster** — second advertising instance broadcasts current
  timestamp so `ATC_MiThermometer` devices auto-sync (port of `ha-atc-time-sync`).
- **HTTP server + LittleFS** — admin Web UI with worker management, pairing
  windows, calendar view, CSV export.
- **NVS** — bond keys (LTK/IRK), config, admin password hash.
- **NTP** — `esp_sntp` keeps system clock; events use monotonic clock + backfill
  if NTP not yet synced.

## Build & Flash

Requires:
- Docker (OrbStack/Desktop) — runs `espressif/idf:v6.0.1`
- `espflash` on host — `brew install espflash` (USB passthrough doesn't work
  inside macOS Docker)

```bash
make build        # compile in Docker
make flash        # flash via espflash on host
make monitor      # serial log via espflash
make flash-monitor

make menuconfig   # interactive sdkconfig
make erase        # nuke flash
make fullclean    # nuke build/ + sdkconfig
```

Default port `/dev/cu.usbmodem101` — override with `make flash PORT=/dev/cu.xxx`.
