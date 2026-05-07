# T-CAN485 Tank Bridge

ESP32 firmware that reads a water-level sensor over RS485 (Modbus RTU) and
republishes the value to a Victron Cerbo GX two ways:

- **NMEA 2000** — PGN 127505 (Fluid Level, type Water) over VE.Can.
- **BLE** — spoofed Mopeka Pro Check H2O advertisement, so the Cerbo GX
  discovers it as a Bluetooth tank sensor.

Either path can be enabled independently from the web UI.

## Hardware

[LilyGo T-CAN485](https://github.com/Xinyuan-LilyGO/T-CAN485) — ESP32 board
with:

- CAN bus (SN65HVD231 via TWAI)
- RS485 (MAX13487EESA+)
- WS2812B status LED
- SD card slot (unused by this firmware)
- WiFi + BLE

The vendored upstream LilyGo repo lives at `T-CAN485/` as a git submodule
(pinned). We pull `pin_config.h` and FastLED from its `lib/` via PlatformIO's
`lib_extra_dirs`.

## Layout

```
canrs485/
├── platformio.ini
├── src/                Tank Bridge firmware
├── scripts/            build_index_html.py pre-build hook
├── docs/               sensor + Cerbo GX reference PDFs
└── T-CAN485/           submodule, vendored upstream (pin_config.h, FastLED)
```

## Build

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html).

```bash
git clone --recursive https://github.com/metasov/T-CAN485-cerbo-water-tank.git
cd T-CAN485-cerbo-water-tank
pio run                          # build
pio run --target upload          # build + flash over USB
pio device monitor               # serial monitor at 115200 baud
```

If you cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

### First flash

NVS persists across re-uploads. To wipe saved WiFi creds and start with the
default password regenerated at boot:

```bash
pio run -t erase && pio run -t upload
```

If flashing fails, hold **BOOT-0**, briefly press **EN/RST** (release RST,
keep holding BOOT-0), release BOOT-0 a moment later, then retry. Close the
serial monitor first.

## First-boot setup

1. The board comes up as a WiFi access point named **TankBridge-XXXX**. The
   admin password prints on the serial console at 115200 baud (regenerated on
   every boot until you change it via the web UI).
2. Connect to the AP — your phone or laptop is captive-portal-redirected to
   the setup page.
3. Pick your home WiFi from the scan list, enter its password, save.
4. Once the board joins your network, reach the dashboard at
   **http://watertank.local/** (mDNS).
5. Sign in with `admin` and the password from the serial console; change it
   from the dashboard.
6. Configure the sensor: tank capacity, sensor range, Modbus address. Use the
   **Probe** wizard if you don't know the protocol.
7. Enable NMEA 2000 and/or BLE output as desired.

## OTA updates

Once the firmware is running, subsequent updates can be pushed over WiFi:

```bash
pio run
curl -u admin:<password> -F "file=@.pio/build/esp32dev/firmware.bin" \
     http://watertank.local/api/ota
```

The device reboots ~500 ms after the response. NVS settings survive the
update.

## Documentation

- `CLAUDE.md` — architecture, pin map, gotchas (intended for AI coding
  assistants but also useful as a developer reference).
- `docs/docs/mopeka_ble_protocol.md` — reverse-engineered Mopeka
  Pro Check H2O BLE advertisement format consumed by Victron's
  `dbus-ble-sensors`.
- `docs/sensor.pdf` — water-level sensor datasheet.
- `docs/140558-Ekrano_GX__Venus_GX__Cerbo_GX__Cerbo-S_GX_Manual-pdf-en.pdf` —
  Cerbo GX manual (NMEA 2000 + Bluetooth tank-sensor sections).

## License

[MIT](LICENSE).
