#pragma once

#include <Arduino.h>

namespace cfg {

constexpr size_t PWD_SALT_LEN = 16;
constexpr size_t PWD_HASH_LEN = 32;  // SHA-256

struct Settings {
    // WiFi STA
    char wifi_ssid[33] = {0};
    char wifi_pass[65] = {0};

    // Tank
    uint32_t tank_capacity_l = 200;        // litres
    uint16_t sensor_range_mm = 5000;       // 0..range maps to 0..100%

    // Modbus
    uint8_t  modbus_slave    = 1;
    uint32_t modbus_baud     = 9600;
    uint8_t  modbus_parity   = 0;          // 0=N, 1=E, 2=O
    uint8_t  modbus_fn       = 0x03;       // 0x03 read holding, 0x04 read input
    uint16_t modbus_reg      = 0x0004;
    float    modbus_scale    = 1.0f;       // raw * scale = mm
    uint16_t poll_ms         = 1000;

    // CAN bus
    bool     can_enabled     = true;

    // BLE (Mopeka Pro H2O emulation)
    bool     ble_enabled     = false;

    // Admin
    uint8_t  pwd_salt[PWD_SALT_LEN] = {0};
    uint8_t  pwd_hash[PWD_HASH_LEN] = {0};
    bool     pwd_set = false;        // any password (default or user) is hashed
    bool     pwd_is_default = true;  // true until user changes it via web UI
};

void begin();
const Settings& get();
void save(const Settings& s);
void factoryReset();

}  // namespace cfg
