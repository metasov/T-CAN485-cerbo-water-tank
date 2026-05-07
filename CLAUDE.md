# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Firmware for the **LilyGo T-CAN485** — an ESP32-based board with CAN bus (SN65HVD231 via TWAI), RS485 (MAX13487EESA+), WS2812B RGB LED, SD card, and WiFi.

**Active project:** the **T-CAN485 Tank Bridge** firmware in `src/`. It reads a water-level sensor over RS485 (Modbus RTU) and republishes the value two ways: as NMEA 2000 PGN 127505 (Fluid Level, type Water) to a Victron Cerbo GX over VE.Can, and/or as a spoofed Mopeka Pro Check H2O BLE advertisement so the Cerbo GX discovers it as a Bluetooth tank sensor. Includes a web UI (`http://watertank.local/`) with captive-portal WiFi setup, mDNS, admin auth, and a Modbus probe mode for unknown sensors. The plan file lives at `/Users/archim/.claude/plans/shimmying-brewing-shannon.md`.

## Layout

This repo is its own PlatformIO project at the root. The vendored LilyGo T-CAN485 reference repo is a git submodule at `T-CAN485/`, pinned to a specific upstream commit; `platformio.ini` sets `lib_extra_dirs = T-CAN485/lib` so `pin_config.h` and FastLED resolve out of it.

```
canrs485/                  ← repo root, PlatformIO project
├── platformio.ini
├── src/                   ← our firmware
├── scripts/               ← build_index_html.py pre-build hook
├── docs/
└── T-CAN485/              ← submodule (vendored upstream)
    ├── examples/          ← stock LilyGo reference code, not built
    └── lib/Mylibrary/pin_config.h
```

The `T-CAN485/examples/` tree is stock LilyGo reference code, **not built**; the plain examples below describe what each one does for reference.

## Build System: PlatformIO

All commands run from the repo root:

```bash
pio run                          # build
pio run --target upload          # build + flash
pio device monitor               # open serial monitor (115200 baud)
pio run --target upload && pio device monitor  # flash then monitor
pio run --target clean           # clean build artifacts
```

**Default `src_dir` is `src/` (the Tank Bridge firmware).** To temporarily build a stock example instead, add `src_dir = T-CAN485/examples/<name>` under `[platformio]`.

If flashing fails, hold the **BOOT-0** button on the board, briefly press **EN/RST** (release RST, keep holding BOOT-0), release BOOT-0 a moment later, then retry. The serial monitor must be closed first — only one process can hold the port.

**NVS persists across re-uploads.** Use `pio run -t erase` to wipe NVS (e.g. to clear saved WiFi creds and trigger first-boot setup again).

## Architecture

### Tank Bridge firmware (`src/`)

| File | Responsibility |
|------|----------------|
| `main.cpp` | Hardware enable sequence, module init, cooperative loop |
| `Config.h/cpp` | NVS-backed settings (WiFi, sensor, tank, password salt+hash) |
| `Auth.h/cpp` | mbedtls SHA-256 password hashing, HTTP Basic helper |
| `Sensor.h/cpp` | Modbus RTU master on Serial1; two-phase probe in a FreeRTOS task |
| `N2k.h/cpp` | NMEA 2000 wrapper (uses `NMEA2000_esp32_twai` fork); PGN 127505 every 2.5 s |
| `WifiMgr.h/cpp` | AP+STA, captive-portal DNS, scan, mDNS as `watertank.local`, AP shutdown 30 s after STA up |
| `WebUi.h/cpp` | `ESPAsyncWebServer` routes — public reads, `admin`-Basic-auth POSTs |
| `index_html.h` | PROGMEM dashboard + setup view (WiFi network selector pre-config) |
| `Led.h/cpp` | WS2812 status colors (green=publishing, yellow=sensor-only, red=err, blue=probing) |
| `Ble.h/cpp` | Mopeka Pro Check H2O BLE advertisement emulation; publishes level every 2.5 s |

`platformio.ini` lib_deps: `ttlappalainen/NMEA2000-library`, the `NMEA2000_esp32_twai` git fork, `esp32async/ESPAsyncWebServer` + `AsyncTCP`, `ArduinoJson` v7, `ESP32 BLE Arduino`. **`-DESP32_CAN_TX_PIN=GPIO_NUM_27 -DESP32_CAN_RX_PIN=GPIO_NUM_26`** is critical — without it the NMEA 2000 fork tries GPIO 32/34 (or upstream tries GPIO 16, which is `ME2107_EN` on this board).

Don't `#include "driver/twai.h"` from app code — the NMEA 2000 lib owns the CAN peripheral.

### BLE / Mopeka Emulation (`Ble.h/cpp`)

The firmware optionally advertises as a **Mopeka Pro Check H2O** sensor (`ble_enabled` config flag, default off). When enabled, `BLEDevice::init("Pro+")` is called at startup (takes ~1 s, uses ~80–100 KB of heap for the BLE stack), and `loop()` fires a non-connectable advertisement every 2.5 s once the first Modbus reading arrives.

**How Cerbo GX / VenusOS `dbus-ble-sensors` detects Mopeka sensors** (C daemon, not Python):
- Looks only at AD type `0xFF` (Manufacturer Specific Data). Service UUID and device name are ignored.
- Triggers on company ID `0x0059` (Nordic Semiconductor — Mopeka uses nRF52).
- Applies three hard checks on the 10-byte payload (after the 2-byte company ID):

| Payload offset | Field | Our value |
|---------------|-------|-----------|
| 0 | Model byte — must be in allowlist | `0x05` (Pro Check H2O) |
| 1 | Battery (7-bit raw, bit7=button) | `0x7F` (fixed ~3.97 V) |
| 2 | Temperature (bits6:0 = °C+40, bit7=sync) | `0x28` (fixed 0 °C) |
| 3–4 | Level mm, 14-bit LE; upper 2 bits = quality | computed |
| 5–7 | **UID = `addr->b[2..0]`** (must match BLE src addr) | cached at init |
| 8–9 | Accelerometer X/Y | `0x00 0x00` |

**Model byte allowlist** (any other value → silent discard): `0x03` LPG, `0x04` Pro-200 top-down, `0x05` H2O, `0x08` Pro+ BLE, `0x09` Pro+ Cellular, `0x0A`/`0x0B` TD Topdown, `0x0C` Universal, `0x12` Pro-200B.

**Critical: the UID field.** `dbus-ble-sensors` compares payload bytes 5–7 against `addr->b[2], addr->b[1], addr->b[0]` of the BLE advertisement's source MAC. A mismatch → silent packet discard. `Ble.cpp` caches these bytes from `BLEDevice::getAddress().getNative()` at `begin()`.

**Heap impact.** The BLE stack allocation (~80–100 KB) leaves insufficient contiguous heap for `ESPAsyncWebServer`'s `beginResponse(code, type, const char*)`, which tries to copy the ~57 KB `INDEX_HTML` string into a `String`. The `/` route therefore uses `beginChunkedResponse` to stream from flash in chunks.

### Pin Definitions
All GPIO assignments live in `T-CAN485/lib/Mylibrary/pin_config.h` (in the vendored submodule, reachable via `lib_extra_dirs`). Always `#include "pin_config.h"` rather than hardcoding pins. Note: the README's PinOverview lists RS485_EN as IO9, but the actual header and code use **IO19**.

### Hardware Enable Sequence
The ME2107 boost converter (`ME2107_EN`, IO16) **must be set HIGH** before using CAN or RS485 — both buses require boosted voltage. Every example does this in `setup()`.

### CAN Bus
Uses the ESP32 native **TWAI driver** (`#include "driver/twai.h"`), not a third-party CAN library. Default speed: 500 Kbits/s. `CAN_SPEED_MODE` (IO23) set LOW = high-speed mode.

### RS485
Uses `HardwareSerial` (Serial1) on IO21/IO22. The MAX13487EESA+ has two control pins:
- `RS485_EN` (IO19): HIGH = chip enabled
- `RS485_CALLBACK` (IO17): RE pin — HIGH = receive enabled, LOW = transmit enabled

The `RS485.ino` example sends a **Modbus RTU** read-holding-registers request and prints the raw hex response.

### Libraries
- `FastLED-3.7.0` — bundled in `T-CAN485/lib/` (vendored), controls the WS2812B LED (GRB ordering, DATA pin IO4)
- All other dependencies are framework built-ins (WiFi, HTTPClient, SD, SPI, HardwareSerial, TWAI)

### Examples Summary
| Example | Purpose |
|---------|---------|
| `CAN` | TWAI send/receive loop at 500 Kbits/s |
| `RS485` | Modbus RTU polling (reads 4 holding registers from device 0x01) |
| `RS485_WS2812B` | RS485 + LED combined |
| `Original_Test` | Factory test: CAN + RS485 + WiFi + SD all together |
| `SD` | SD card read/write |
| `WIFI_HTTP_Download_File` | WiFi HTTP download speed test |
| `WS2812B_Blink` | LED blink |

## Common gotchas (learned the hard way)

- **Bad USB-C cable** is the #1 cause of "no `/dev/cu.usbserial-*` appears". Try a known-good phone-data cable.
- **`N2kft_FreshWater` doesn't exist** in the NMEA2000 lib — use `N2kft_Water` (=1).
- **HTML `[hidden]` attribute is overridden by author CSS** like `main { display: grid }` (author > UA in cascade). Add `main[hidden] { display: none; }` if you use the attribute on a styled tag.
- **Don't gate POST endpoints with HTTP Basic Auth** when the body already carries credentials (e.g. password change with `{old, new}`). Browser pops two prompts — bad UX.
- **BOOT-0 LOW at reset** = serial download mode; app code never sees it. For app-time long-press recovery, use a different GPIO or print the recovery secret on every boot until the user changes it.
- **Mopeka model byte `0x04` ≠ H2O.** `0x04` is "Pro-200 top-down"; the H2O / Pro Check Water sensor is `0x05`. Using the wrong byte causes `dbus-ble-sensors` to silently discard the packet.
- **Mopeka UID bytes must match the BLE source MAC.** `dbus-ble-sensors` rejects any Mopeka advertisement where payload bytes 5–7 don't equal `addr->b[2..0]` of the advertising device's MAC. Always populate these from `BLEDevice::getAddress().getNative()` at init — never leave them as zeros.
- **BLE stack + large PROGMEM HTML = heap exhaustion.** `beginResponse(code, type, const char*)` copies the full HTML into a heap `String`. With BLE enabled (~80–100 KB heap consumed), this silently fails → `Content-Length: 0`. Use `beginChunkedResponse` to stream `INDEX_HTML` directly from flash.
