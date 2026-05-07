# Mopeka Pro BLE Advertisement Protocol

Reference for emulating a Mopeka Pro sensor on ESP32 so Victron Venus OS recognizes it as a tank level source.

> **Provenance.** Mopeka Products LLC has not published this protocol officially. The byte layout below was derived from the open-source consumer side ([`victronenergy/dbus-ble-sensors`](https://github.com/victronenergy/dbus-ble-sensors)) and from prior community reverse-engineering — see [References](#references) for the chain of work this document distils. This document and the firmware that uses it are not affiliated with or endorsed by Mopeka.

---

## Overview

Mopeka Pro sensors broadcast BLE advertisements passively — no GATT connection, no pairing. The Cerbo GX (Venus OS ≥ 2.90) scans for these and registers them as tank level inputs. To emulate one, you reproduce the advertisement format exactly.

---

## Advertisement Structure

| Field | Value |
|-------|-------|
| Type | ADV_NONCONN_IND (non-connectable undirected) |
| Interval | ~3.5 s normal; ~333 ms for 20 min after SYNC button press |
| Service UUID | `0xADA0` (checked by HA/Python path; **ignored by dbus-ble-sensors**) |
| Company ID | `0x0059` (Nordic Semiconductor, little-endian) |
| Manufacturer data | 10 bytes (after company ID) |

The advertisement must include:
- A manufacturer-specific data record (AD type `0xFF`) with company ID `0x0059` followed by the 10 payload bytes below

The service UUID record (`0xADA0`) is checked by the Home Assistant Python library but **not** by the Victron `dbus-ble-sensors` C daemon on the Cerbo GX. Include it anyway for cross-compatibility.

---

## Manufacturer Data Payload (10 bytes, after company ID)

```
Byte 0:   [7:0]  Sensor model identifier (see Model Byte table) — validated against allowlist
Byte 1:   [6:0]  Battery raw (7 bits)
          [7]    Reserved / button state (varies by firmware)
Byte 2:   [6:0]  Temperature raw (7 bits)
          [7]    SYNC button pressed flag
Byte 3:   [7:0]  Level low byte
Byte 4:   [5:0]  Level high 6 bits (bits [13:8] of level)
          [7:6]  Read quality (2 bits, 0–3)
Bytes 5–7:       UID — must equal addr->b[2], addr->b[1], addr->b[0] (see UID section)
Bytes 8–9:       Accelerometer X/Y (zero-fill for emulation)
```

### Level field (bytes 3–4, 14-bit little-endian)

```
uint16_t raw      = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
uint16_t level_mm = raw & 0x3FFF;       // bits 13:0
uint8_t  quality  = (raw >> 14) & 0x3;  // bits 15:14
```

`level_mm` is the ultrasonic echo distance (sensor-to-liquid surface), **not** remaining fill height. Full tank = small number; empty = large number. Victron uses the configured empty/full thresholds to compute percentage.

### Quality field

| Value | Meaning |
|-------|---------|
| 0 | No echo (treat as unreliable) |
| 1 | Low confidence |
| 2 | Medium confidence |
| 3 | High confidence |

Set quality = 3 for synthetic data. If the upstream sensor read fails, drop to 0.

### Battery encoding

```
uint8_t raw_batt  = data[1] & 0x7F;
float   voltage_V = raw_batt / 32.0f;           // e.g. 80 → 2.5 V
uint8_t pct       = ((voltage_V - 2.2f) / 0.65f) * 100;  // clamped 0–100
```

For emulation, `0x7F` encodes ~3.97 V (always-on device).

### Temperature encoding

```
uint8_t raw_temp = data[2] & 0x7F;
int8_t  temp_C   = (int8_t)raw_temp - 40;   // e.g. 70 → 30 °C
```

For emulation: `data[2] = (temp_C + 40) & 0x7F`. Use `0x28` (40 dec) for 0 °C fixed.

### SYNC button bit

Bit 7 of byte 2. Set to 0 for emulation.

---

## UID Field (bytes 5–7) — CRITICAL

`dbus-ble-sensors` performs a self-authentication check: it compares payload bytes 5–7 against the last three bytes of the BLE source MAC address in `esp_bd_addr_t` order:

```c
// dbus-ble-sensors check (from ble-scan.c):
if (uid[0] != addr->b[2] ||
    uid[1] != addr->b[1] ||
    uid[2] != addr->b[0])
    return -1;  // packet silently discarded
```

`esp_bd_addr_t` stores the MAC LSB-first, so for MAC `AA:BB:CC:DD:EE:FF`:
- `b[0]` = `0xFF`, `b[1]` = `0xEE`, `b[2]` = `0xDD`
- UID bytes 5–7 must be: `0xDD`, `0xEE`, `0xFF`

In other words, **bytes 5–7 = the last three bytes of the MAC in normal human-readable order**.

On ESP32 with the Arduino BLE library:
```cpp
const uint8_t* mac = (const uint8_t*)BLEDevice::getAddress().getNative();
data[5] = mac[2];  // addr->b[2]
data[6] = mac[1];  // addr->b[1]
data[7] = mac[0];  // addr->b[0]
```

**Never leave bytes 5–7 as zeros** — the packet will always be rejected.

---

## Model Byte (byte 0)

`dbus-ble-sensors` validates the model byte against an explicit allowlist. Any unrecognised value → silent discard.

| Byte 0 | Variant | Notes |
|--------|---------|-------|
| `0x03` | Pro Check LPG | |
| `0x04` | Pro-200 (top-down) | **Not H2O** — do not use for water tank emulation |
| `0x05` | **Pro Check H2O (Water)** | Correct choice for water tank |
| `0x08` | Pro+ BLE | |
| `0x09` | Pro+ Cellular | |
| `0x0A` | TD Topdown BLE | |
| `0x0B` | TD Topdown Cellular | |
| `0x0C` | Pro Check Universal | |
| `0x12` | Pro-200B | |

> `0x04` and `0x05` look similar but are different products. The T-CAN485 Tank Bridge uses `0x05`.

---

## Victron Venus OS Support Timeline

| Venus OS | What was added |
|----------|----------------|
| v2.90 | Pro H2O, Pro LPG |
| v3.14 | Universal, TD40, Pro 200, Pro+ |
| v3.70 | Tank shape config, high/low alarms |

Passive BLE scanning only — the GX device never initiates a connection.

---

## Full Emulation Packet Example

Goal: water tank at 350 mm echo distance, 25 °C, ~3.97 V battery, quality 3, MAC `AA:BB:CC:DD:EE:FF`.

```
Model byte  (data[0]): 0x05          // Pro Check H2O
Battery     (data[1]): 0x7F          // ~3.97 V (fixed, always-on device)
Temp+button (data[2]): 0x41          // (25+40)=65=0x41, button=0
Level low   (data[3]): 0x5E          // 350 = 0x015E → low byte = 0x5E
Level high  (data[4]): 0xC1          // high byte=0x01, quality=3 → (3<<6)|0x01 = 0xC1
UID         (data[5]): 0xDD          // mac[2] (addr->b[2])
UID         (data[6]): 0xEE          // mac[1] (addr->b[1])
UID         (data[7]): 0xFF          // mac[0] (addr->b[0])
Accel       (data[8–9]): 0x00 0x00
```

Level encoding detail:
```
uint16_t level = 350;              // mm
uint8_t  quality = 3;
uint16_t packed = level | ((uint16_t)quality << 14);  // 0xC15E
data[3] = packed & 0xFF;           // 0x5E
data[4] = (packed >> 8) & 0xFF;    // 0xC1
```

---

## ESP32 BLE Advertising (Arduino / esp-idf)

```cpp
#include <BLEDevice.h>
#include <BLEAdvertising.h>

static uint8_t g_uid[3];  // cached MAC bytes for UID field

// Call once in setup(), after ME2107_EN is HIGH
void mopeka_ble_init() {
    BLEDevice::init("Pro+");
    // Cache UID bytes — dbus-ble-sensors checks these against the BLE source address
    const uint8_t* mac = (const uint8_t*)BLEDevice::getAddress().getNative();
    g_uid[0] = mac[2]; g_uid[1] = mac[1]; g_uid[2] = mac[0];
}

// Call whenever level changes (or on a ~2.5 s timer), after first valid reading
void mopeka_ble_update(uint16_t level_mm, int8_t temp_c, uint8_t quality) {
    uint8_t data[10] = {};
    data[0] = 0x05;                                   // Pro Check H2O
    data[1] = 0x7F;                                   // ~3.97 V battery
    data[2] = (uint8_t)((temp_c + 40) & 0x7F);
    uint16_t packed = level_mm | ((uint16_t)(quality & 0x3) << 14);
    data[3] = packed & 0xFF;
    data[4] = (packed >> 8) & 0xFF;
    data[5] = g_uid[0]; data[6] = g_uid[1]; data[7] = g_uid[2];

    std::string mfr;
    mfr += (char)0x59; mfr += (char)0x00;  // company ID 0x0059 little-endian
    mfr.append((char*)data, 10);

    BLEAdvertisementData adv_data;
    adv_data.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    adv_data.setName("Pro+");
    adv_data.setCompleteServices(BLEUUID((uint16_t)0xADA0));
    adv_data.setManufacturerData(mfr);

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->stop();
    adv->setAdvertisementData(adv_data);
    adv->setAdvertisementType(ADV_TYPE_NONCONN_IND);
    adv->start();
}
```

> **Heap note:** `BLEDevice::init()` consumes ~80–100 KB of heap for the BLE controller stack. If the web server serves a large HTML page via `beginResponse(code, type, const char*)`, the String allocation will silently fail (Content-Length: 0). Use `beginChunkedResponse` to stream PROGMEM content instead.

---

## Debugging on Cerbo GX

If the sensor doesn't appear in Settings → Integrations → Bluetooth Sensors:

```bash
# SSH into Cerbo, check if advertisement reaches the BT adapter:
hcitool lescan --passive

# Watch dbus-ble-sensors log:
journalctl -f -u dbus-ble-sensors
# or on older Venus:
logread -f | grep ble
```

If the MAC appears in `hcitool lescan` but not in the Bluetooth Sensors tab, the packet is being parsed but rejected — check model byte and UID field.

---

## Limitations and Risks

- **Range**: BLE is ~10 m line-of-sight. The Cerbo GX must be within range.
- **Protocol stability**: Mopeka's byte layout is reverse-engineered from `dbus-ble-sensors` source and community work, not officially documented. A VenusOS update could tighten validation.
- **Tank calibration lives in the GX**: The Cerbo GX applies empty/full mm thresholds and tank shape correction — the emulator emits raw echo distance only.
- **No acknowledgment**: The GX silently ignores packets it rejects.

---

## References

The byte layout above was assembled by cross-referencing the following projects. Each is a primary source for one or more of the fields documented here.

- [`victronenergy/dbus-ble-sensors`](https://github.com/victronenergy/dbus-ble-sensors) —
  the Cerbo GX C daemon that consumes these advertisements. `ble-scan.c` is
  the **definitive** source for what the GX accepts: model byte allowlist,
  the UID self-authentication check, and the company-ID filter.
- [`spbrogan/mopeka_pro_check`](https://github.com/spbrogan/mopeka_pro_check) —
  one of the earliest public Python decoders; primary source for the
  battery-voltage and temperature encodings.
- [ESPHome `mopeka_pro_check` component](https://esphome.io/components/sensor/mopeka_pro_check/) —
  C++ parser; cross-checked against the level/quality bit packing.
- [`Bluetooth-Devices/mopeka-iot-ble`](https://github.com/Bluetooth-Devices/mopeka-iot-ble) —
  Home Assistant integration path; validates the `0xADA0` service UUID record
  (which `dbus-ble-sensors` ignores but HA requires).
- [Theengs Decoder — Mopeka entry](https://decoder.theengs.io/devices/Mopeka.html) —
  community decoder catalogue; convenient cross-reference for variant model bytes.
- [Victron Community: Mopeka BLE tank sensors thread](https://community.victronenergy.com/questions/52274/venus-raspberry-pi-read-other-ble-device.html) —
  field reports on Venus OS support across versions.
- Victron Venus OS [v2.90 release notes](https://www.victronenergy.com/live/venus-os:start) —
  initial Mopeka Pro support announcement.

> The author is not affiliated with Mopeka Products LLC. "Mopeka" and "Pro Check" are trademarks of their respective owners.
