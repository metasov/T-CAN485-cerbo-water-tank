#include "Ble.h"
#include "Config.h"

#include <BLEDevice.h>
#include <BLEAdvertising.h>

namespace ble {

namespace {

constexpr uint16_t SERVICE_UUID = 0xADA0;
constexpr uint16_t COMPANY_ID   = 0x0059;
constexpr uint16_t ADV_INTERVAL = 0x0640;  // 1000 ms (units 0.625 ms)
constexpr uint32_t UPDATE_MS    = 2500;

// dbus-ble-sensors converts raw Mopeka level to mm using:
//   scale = 0.600592 + 0.003124*t - 0.00001368*t²  (H2O model coefs)
// where t is the raw temperature byte value (bits[6:0]), NOT degrees Celsius.
// Our temp byte is 0x28 = 40 (encodes 0°C as 0+40=40), giving:
//   scale = 0.600592 + 0.003124*40 - 0.00001368*1600 = 0.703664
// Pre-divide physical mm by this scale so Cerbo's output equals sensor depth.
constexpr float MOPEKA_LEVEL_FACTOR = 0.703664f;

bool     g_disabled    = false;
float    g_level_mm    = -1.0f;  // sentinel: no reading yet
uint32_t g_tx_count    = 0;
uint32_t g_next_update = 0;
uint8_t  g_uid[3]      = {};     // MAC bytes b[2..0], cached at init for Mopeka UID check

BLEAdvertising* g_adv = nullptr;

// 12-byte manufacturer payload: [company_id×2, model, battery, temp, level×2, uid×3, accel×2]
// dbus-ble-sensors validates: model in allowlist, len==10 after company_id, uid==addr->b[2..0].
// dbus-ble-sensors multiplies the raw level field by MOPEKA_LEVEL_FACTOR (0.704) to get physical mm,
// so we pre-divide to compensate: raw = physical_mm / 0.704.
std::string buildMfgData(float level_mm_physical) {
    uint16_t mopeka_raw = (uint16_t)constrain(
        level_mm_physical / MOPEKA_LEVEL_FACTOR, 0.0f, 16383.0f);
    // bits [13:0] = mopeka_raw, bits [15:14] = quality (3 = high)
    uint16_t packed = mopeka_raw | ((uint16_t)3 << 14);
    char buf[12] = {
        (char)(COMPANY_ID & 0xFF), (char)((COMPANY_ID >> 8) & 0xFF),
        0x05,  // model: Pro Check H2O (Water) — dbus-ble-sensors allowlist
        0x7F,  // battery fixed ~3.97 V (7-bit; bit 7 = 0, no button)
        0x28,  // temp fixed 0 °C encoded as 0+40=40=0x28 (bit 7 = 0, no SYNC)
        (char)(packed & 0xFF),
        (char)((packed >> 8) & 0xFF),
        (char)g_uid[0], (char)g_uid[1], (char)g_uid[2],  // must match BLE addr b[2..0]
        0, 0  // accelerometer
    };
    return std::string(buf, 12);
}

void applyAdvertisement(float level_mm_physical) {
    g_adv->stop();
    BLEAdvertisementData advData;
    advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    advData.setName("Pro+");
    advData.setCompleteServices(BLEUUID((uint16_t)SERVICE_UUID));
    advData.setManufacturerData(buildMfgData(level_mm_physical));
    g_adv->setAdvertisementData(advData);
    g_adv->setAdvertisementType(ADV_TYPE_NONCONN_IND);
    g_adv->setMinInterval(ADV_INTERVAL);
    g_adv->setMaxInterval(ADV_INTERVAL);
    g_adv->start();
    ++g_tx_count;
}

}  // namespace

void begin() {
    if (!cfg::get().ble_enabled) { g_disabled = true; return; }
    // init() also calls esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT) internally.
    BLEDevice::init("Pro+");
    g_adv = BLEDevice::getAdvertising();
    // Cache addr->b[2..0] — dbus-ble-sensors checks these against UID bytes 5-7 of payload.
    // getNative() returns MSB-first: [2C,BC,BB,A8,0C,26] for 2C:BC:BB:A8:0C:26.
    // dbus-ble-sensors checks uid[0..2] == bdaddr.b[2..0] (BlueZ LSB-first),
    // which equals mac[3..5] in MSB-first notation.
    const uint8_t* mac = (const uint8_t*)BLEDevice::getAddress().getNative();
    g_uid[0] = mac[3]; g_uid[1] = mac[4]; g_uid[2] = mac[5];
    Serial.printf("[ble] advertising as Mopeka Pro H2O, uid=%02x:%02x:%02x\n",
                  g_uid[0], g_uid[1], g_uid[2]);
    // First advertisement fires from loop() once sensor delivers a reading.
}

void publishLevel(float level_mm) {
    if (g_disabled) return;
    g_level_mm = level_mm < 0.0f ? 0.0f : level_mm;
}

void loop() {
    if (g_disabled || !g_adv) return;
    if (g_level_mm < 0.0f) return;  // no reading yet
    uint32_t now = millis();
    if ((int32_t)(now - g_next_update) < 0) return;
    g_next_update = now + UPDATE_MS;
    applyAdvertisement(g_level_mm);
}

bool     active()     { return !g_disabled && g_adv != nullptr; }
uint32_t txCount()   { return g_tx_count; }
String   macAddress() {
    if (g_disabled || !g_adv) return "";
    return String(BLEDevice::getAddress().toString().c_str());
}

}  // namespace ble
