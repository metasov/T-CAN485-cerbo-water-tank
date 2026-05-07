#include "WifiMgr.h"
#include "Config.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_mac.h>

namespace wifimgr {

namespace {
constexpr uint32_t RECONNECT_BACKOFF_MS = 5000;
constexpr uint32_t AP_GRACE_MS          = 30000;  // wait this long after STA-up before tearing AP down
constexpr uint32_t AP_RECOVERY_MS       = 30000;  // wait this long after STA-down before bringing AP back
constexpr uint32_t AP_RETRY_MS          = 5000;   // retry apUp() if softAP() failed
constexpr uint8_t  DNS_PORT             = 53;

String     g_ap_ssid;
StaState   g_sta = StaState::Disabled;
uint32_t   g_next_attempt = 0;
uint32_t   g_next_ap_retry = 0;

bool       g_ap_active = false;
DNSServer  g_dns;

bool       g_was_connected = false;
uint32_t   g_sta_connected_since = 0;
uint32_t   g_sta_lost_since = 0;
bool       g_mdns_started = false;

// Pending creds — held in RAM only until the first successful WL_CONNECTED.
// Avoids poisoning NVS with a typo'd password.
String     g_pending_ssid;
String     g_pending_pass;
bool       g_creds_pending_save = false;
bool       g_creds_changed = false;
uint32_t   g_creds_changed_at = 0;
const char* g_sta_reason = "";
constexpr uint32_t SETTLE_MS = 250;

// Forward decls — apDown() needs mdnsStop().
void mdnsStop();

String macSuffix() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char buf[5];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return String(buf);
}

void apUp() {
    if (g_ap_active) return;
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(g_ap_ssid.c_str())) {
        Serial.println(F("[wifi] softAP() failed; will retry"));
        g_next_ap_retry = millis() + AP_RETRY_MS;
        return;
    }
    delay(50);
    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    if (!g_dns.start(DNS_PORT, "*", WiFi.softAPIP())) {
        Serial.println(F("[wifi] DNS start failed; AP up but no captive portal"));
    }
    g_ap_active = true;
    g_next_ap_retry = 0;
}

void apDown() {
    if (!g_ap_active) return;
    g_dns.stop();
    // ESPmDNS binds to the netif that exists at MDNS.begin() time. Switching
    // the WiFi mode flips the underlying netif and leaves the responder in a
    // half-broken state — watertank.local stops resolving even though /api is
    // reachable by IP. Stop mDNS here; loop()'s mdnsStart() rebinds on the
    // pure-STA netif on the next tick.
    mdnsStop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_ap_active = false;
}

void mdnsStart() {
    if (g_mdns_started) return;
    // Belt-and-braces: free any stale state from a prior begin() that's no
    // longer bound to the current netif (e.g. after AP_STA → STA mode flip).
    MDNS.end();
    if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        g_mdns_started = true;
    }
}

void mdnsStop() {
    if (!g_mdns_started) return;
    MDNS.end();
    g_mdns_started = false;
}

void startStaIfConfigured() {
    const char* ssid;
    const char* pass;
    if (g_creds_pending_save) {
        ssid = g_pending_ssid.c_str();
        pass = g_pending_pass.c_str();
    } else {
        const cfg::Settings& s = cfg::get();
        if (s.wifi_ssid[0] == 0) {
            g_sta = StaState::Disabled;
            return;
        }
        ssid = s.wifi_ssid;
        pass = s.wifi_pass;
    }
    g_sta = StaState::Connecting;
    WiFi.begin(ssid, pass);
    g_next_attempt = millis() + RECONNECT_BACKOFF_MS;
}

}  // namespace

void begin() {
    g_ap_ssid = String("T-CAN485-Tank-") + macSuffix();
    WiFi.setHostname(HOSTNAME);

    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
        if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            uint8_t r = info.wifi_sta_disconnected.reason;
            switch (r) {
                case WIFI_REASON_AUTH_FAIL:
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                    g_sta_reason = "auth_fail";    break;
                case WIFI_REASON_NO_AP_FOUND:
                    g_sta_reason = "no_ap";        break;
                case WIFI_REASON_AUTH_EXPIRE:
                case WIFI_REASON_HANDSHAKE_TIMEOUT:
                    g_sta_reason = "auth_timeout"; break;
                case WIFI_REASON_BEACON_TIMEOUT:
                case WIFI_REASON_ASSOC_TOOMANY:
                    g_sta_reason = "ap_busy";      break;
                default: /* leave whatever was there; transient/uninformative */ break;
            }
        } else if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            g_sta_reason = "";
        }
    });

    apUp();
    startStaIfConfigured();
}

void loop() {
    if (g_ap_active) g_dns.processNextRequest();

    bool connected = (WiFi.status() == WL_CONNECTED);

    // Retry a previously-failed AP bring-up (e.g. softAP() failed at boot).
    // Without this we'd be stranded with neither AP nor STA reachable.
    if (!g_ap_active && !connected && g_next_ap_retry != 0 &&
        (int32_t)(millis() - g_next_ap_retry) >= 0) {
        apUp();
    }

    // Apply a pending credential change. Two-tick state machine: if currently
    // connected, request a disconnect and restart the settle timer; once the
    // disconnect has settled (250 ms), kick off WiFi.begin with the new creds.
    // Avoids the disconnect/begin race that produced spurious connect failed!
    // / AUTH_FAIL / TIMEOUT messages on serial.
    if (g_creds_changed) {
        if (connected) {
            WiFi.disconnect(false, false);
            g_creds_changed_at = millis();
        } else if ((int32_t)(millis() - g_creds_changed_at) >= (int32_t)SETTLE_MS) {
            g_was_connected = false;
            g_sta_connected_since = 0;
            g_sta_lost_since = 0;
            startStaIfConfigured();
            g_creds_changed = false;
        }
        return;
    }

    if (g_sta == StaState::Disabled && !connected) return;

    uint32_t now = millis();

    if (connected) {
        if (!g_was_connected) {
            g_sta_connected_since = now;
            g_sta_lost_since = 0;
            // Commit pending creds to NVS the moment they actually work.
            if (g_creds_pending_save) {
                cfg::Settings s = cfg::get();
                strlcpy(s.wifi_ssid, g_pending_ssid.c_str(), sizeof(s.wifi_ssid));
                strlcpy(s.wifi_pass, g_pending_pass.c_str(), sizeof(s.wifi_pass));
                cfg::save(s);
                g_creds_pending_save = false;
                g_pending_ssid = String();
                g_pending_pass = String();
            }
        }
        g_sta = StaState::Connected;
        g_was_connected = true;

        mdnsStart();

        // Tear AP down once STA has been stable for the grace window.
        if (g_ap_active && (now - g_sta_connected_since) >= AP_GRACE_MS) {
            apDown();
        }
    } else {
        if (g_was_connected) {
            g_sta_lost_since = now;
            g_was_connected = false;
        }
        g_sta = StaState::Connecting;

        // Bring AP back up if STA has been down for the recovery window.
        if (!g_ap_active && g_sta_lost_since != 0 &&
            (now - g_sta_lost_since) >= AP_RECOVERY_MS) {
            mdnsStop();
            apUp();
        }

        // Periodic reconnect attempts.
        if ((int32_t)(now - g_next_attempt) >= 0) {
            g_next_attempt = now + RECONNECT_BACKOFF_MS;
            WiFi.reconnect();
        }
    }
}

void setStaCreds(const String& ssid, const String& pass) {
    // Stage in RAM only. NVS is committed by loop() once the new creds actually
    // produce a WL_CONNECTED — a typo'd password never gets persisted.
    g_pending_ssid = ssid;
    g_pending_pass = pass;
    g_creds_pending_save = true;
    g_sta_reason = "";
    apUp();   // keep captive portal reachable while STA is in flux
    g_creds_changed = true;
    g_creds_changed_at = millis();
}

bool isConfigured() {
    return cfg::get().wifi_ssid[0] != 0;
}

void startScan() {
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
    WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
}

bool scanInProgress() {
    return WiFi.scanComplete() == WIFI_SCAN_RUNNING;
}

std::vector<ScanEntry> scanResults() {
    std::vector<ScanEntry> out;
    int16_t n = WiFi.scanComplete();
    if (n <= 0) return out;
    out.reserve(n);
    for (int16_t i = 0; i < n; ++i) {
        ScanEntry e;
        e.ssid    = WiFi.SSID(i);
        e.rssi    = WiFi.RSSI(i);
        e.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        if (e.ssid.length() > 0) out.push_back(e);
    }
    return out;
}

String      apSsid()   { return g_ap_ssid; }
IPAddress   apIp()     { return g_ap_active ? WiFi.softAPIP() : IPAddress(0, 0, 0, 0); }
StaState    staState() { return g_sta; }
String      staSsid()  {
    // Prefer the pending SSID while a connect attempt is in flight, so the UI
    // can show "connecting to <new network>" instead of the prior NVS value.
    return g_creds_pending_save ? g_pending_ssid : String(cfg::get().wifi_ssid);
}
String      staIp() {
    return g_sta == StaState::Connected ? WiFi.localIP().toString() : String("");
}
const char* staReason() { return g_sta_reason; }

}  // namespace wifimgr
