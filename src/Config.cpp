#include "Config.h"
#include <Preferences.h>

namespace cfg {

namespace {
Preferences prefs;
Settings g_settings;
constexpr const char* NS = "tank";

void writeAll(const Settings& s) {
    prefs.begin(NS, false);
    prefs.putString("ssid",      s.wifi_ssid);
    prefs.putString("pass",      s.wifi_pass);
    prefs.putUInt  ("cap_l",     s.tank_capacity_l);
    prefs.putUShort("range_mm",  s.sensor_range_mm);
    prefs.putUChar ("mb_slave",  s.modbus_slave);
    prefs.putUInt  ("mb_baud",   s.modbus_baud);
    prefs.putUChar ("mb_parity", s.modbus_parity);
    prefs.putUChar ("mb_fn",     s.modbus_fn);
    prefs.putUShort("mb_reg",    s.modbus_reg);
    prefs.putFloat ("mb_scale",  s.modbus_scale);
    prefs.putUShort("poll_ms",   s.poll_ms);
    prefs.putBool  ("can_en",    s.can_enabled);
    prefs.putBool  ("ble_en",    s.ble_enabled);
    prefs.putBytes ("pwd_salt",  s.pwd_salt, PWD_SALT_LEN);
    prefs.putBytes ("pwd_hash",  s.pwd_hash, PWD_HASH_LEN);
    prefs.putBool  ("pwd_set",   s.pwd_set);
    prefs.putBool  ("pwd_def",   s.pwd_is_default);
    prefs.end();
}
}  // namespace

void begin() {
    prefs.begin(NS, true);
    Settings d;  // defaults
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    strlcpy(g_settings.wifi_ssid, ssid.c_str(), sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, pass.c_str(), sizeof(g_settings.wifi_pass));
    g_settings.tank_capacity_l = prefs.getUInt  ("cap_l",     d.tank_capacity_l);
    g_settings.sensor_range_mm = prefs.getUShort("range_mm",  d.sensor_range_mm);
    g_settings.modbus_slave    = prefs.getUChar ("mb_slave",  d.modbus_slave);
    g_settings.modbus_baud     = prefs.getUInt  ("mb_baud",   d.modbus_baud);
    g_settings.modbus_parity   = prefs.getUChar ("mb_parity", d.modbus_parity);
    g_settings.modbus_fn       = prefs.getUChar ("mb_fn",     d.modbus_fn);
    g_settings.modbus_reg      = prefs.getUShort("mb_reg",    d.modbus_reg);
    g_settings.modbus_scale    = prefs.getFloat ("mb_scale",  d.modbus_scale);
    g_settings.poll_ms         = prefs.getUShort("poll_ms",   d.poll_ms);
    g_settings.can_enabled     = prefs.getBool  ("can_en",    d.can_enabled);
    g_settings.ble_enabled     = prefs.getBool  ("ble_en",    d.ble_enabled);
    prefs.getBytes("pwd_salt", g_settings.pwd_salt, PWD_SALT_LEN);
    prefs.getBytes("pwd_hash", g_settings.pwd_hash, PWD_HASH_LEN);
    g_settings.pwd_set = prefs.getBool("pwd_set", false);
    g_settings.pwd_is_default = prefs.getBool("pwd_def", true);
    prefs.end();
}

const Settings& get() { return g_settings; }

void save(const Settings& s) {
    g_settings = s;
    writeAll(g_settings);
}

void factoryReset() {
    prefs.begin(NS, false);
    prefs.clear();
    prefs.end();
    g_settings = Settings{};
}

}  // namespace cfg
