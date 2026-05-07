#include <Arduino.h>
#include <esp_task_wdt.h>

#include "Auth.h"
#include "Ble.h"
#include "Config.h"
#include "Led.h"
#include "N2k.h"
#include "Sensor.h"
#include "WebUi.h"
#include "WifiMgr.h"
#include "pin_config.h"

namespace {

constexpr uint8_t  BOOT_PIN = 0;
constexpr uint32_t FACTORY_HOLD_MS = 5000;
constexpr uint32_t WDT_TIMEOUT_S   = 30;

void enableHardware() {
    pinMode(ME2107_EN, OUTPUT);       digitalWrite(ME2107_EN, HIGH);
    pinMode(CAN_SPEED_MODE, OUTPUT);  digitalWrite(CAN_SPEED_MODE, LOW);
    pinMode(RS485_EN, OUTPUT);        digitalWrite(RS485_EN, HIGH);
    pinMode(RS485_CALLBACK, OUTPUT);  digitalWrite(RS485_CALLBACK, HIGH);
    delay(50);
}

void maybeFactoryReset() {
    pinMode(BOOT_PIN, INPUT_PULLUP);
    if (digitalRead(BOOT_PIN) != LOW) return;
    Serial.println(F("BOOT-0 held — release within 5 s to cancel factory reset"));
    uint32_t t0 = millis();
    while (digitalRead(BOOT_PIN) == LOW) {
        if (millis() - t0 >= FACTORY_HOLD_MS) {
            Serial.println(F("Factory reset!"));
            cfg::factoryReset();
            return;
        }
        delay(50);
    }
}

void updateLed() {
    using S = led::State;
    if (sensor::probeStatus().running)                           { led::set(S::Probing);    return; }
    if (sensor::okCount() == 0 && sensor::errCount() > 0)       { led::set(S::SensorErr);  return; }
    if (sensor::okCount() == 0)                                  { led::set(S::ApOnly);     return; }
    const auto& c = cfg::get();
    if (!c.can_enabled && !c.ble_enabled)                        { led::set(S::SensorOnly); return; }
    if (!c.can_enabled && c.ble_enabled)                         { led::set(S::BleOnly);    return; }
    // CAN enabled. Green only if frames are actually leaving the box —
    // a claimed address on a disconnected CAN bus would otherwise show green.
    if (n2k::ready() && !n2k::busDistressed())                   { led::set(S::Publishing); return; }
    led::set(S::SensorOk);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println(F("T-CAN485 Tank Bridge starting"));

    enableHardware();
    led::begin();

    cfg::begin();
    maybeFactoryReset();
    auth::begin();

    sensor::begin();
    n2k::begin();
    wifimgr::begin();
    ble::begin();
    webui::begin();

    // Reboot if the main loop ever hangs longer than this. The probe task
    // does not subscribe — it has its own bounded inner timeouts.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    Serial.print(F("AP SSID: "));
    Serial.println(wifimgr::apSsid());
}

void loop() {
    esp_task_wdt_reset();

    sensor::poll();

    sensor::Reading r = sensor::last();
    if (r.valid) {
        n2k::publishLevel(r.level_pct, cfg::get().tank_capacity_l);
        ble::publishLevel(r.level_mm);
    }
    n2k::loop();
    ble::loop();

    wifimgr::loop();
    updateLed();
    led::loop();

    delay(2);
}
