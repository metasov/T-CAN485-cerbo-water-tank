#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#include <vector>

namespace wifimgr {

constexpr const char* HOSTNAME = "watertank";


enum class StaState { Disabled, Connecting, Connected };

struct ScanEntry {
    String  ssid;
    int32_t rssi;
    bool    secured;
};

void begin();
void loop();
void setStaCreds(const String& ssid, const String& pass);
bool isConfigured();             // true if STA SSID has been saved at least once

void   startScan();
bool   scanInProgress();
std::vector<ScanEntry> scanResults();

String      apSsid();
IPAddress   apIp();
StaState    staState();
String      staIp();   // empty if not connected
String      staSsid();
const char* staReason();   // "" / "auth_fail" / "no_ap" / "auth_timeout" / "ap_busy"

}  // namespace wifimgr
