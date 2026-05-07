#include "WebUi.h"
#include "Auth.h"
#include "Ble.h"
#include "Config.h"
#include "N2k.h"
#include "Sensor.h"
#include "WifiMgr.h"
#include "index_html.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <freertos/timers.h>

namespace webui {

namespace {
AsyncWebServer g_server(80);

const char* parityName(uint8_t p) {
    return p == 1 ? "8E1" : (p == 2 ? "8O1" : "8N1");
}

const char* staStateName(wifimgr::StaState s) {
    switch (s) {
        case wifimgr::StaState::Connected:  return "connected";
        case wifimgr::StaState::Connecting: return "connecting";
        default: return "disabled";
    }
}

void sendJson(AsyncWebServerRequest* req, JsonDocument& doc, int code = 200) {
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
}

void handleStatus(AsyncWebServerRequest* req) {
    const auto& s = cfg::get();
    sensor::Reading r = sensor::last();
    JsonDocument doc;
    if (r.valid) {
        doc["level_pct"] = r.level_pct;
        doc["level_mm"]  = r.level_mm;
    } else {
        doc["level_pct"] = nullptr;
        doc["level_mm"]  = nullptr;
    }
    doc["last_error"]   = r.last_error ? r.last_error : "";
    doc["sensor_range_mm"] = s.sensor_range_mm;
    doc["capacity_L"]   = s.tank_capacity_l;
    doc["last_read_ms"] = r.ts_ms;
    doc["modbus_ok"]    = sensor::okCount();
    doc["modbus_err"]   = sensor::errCount();
    doc["can_enabled"]    = s.can_enabled;
    doc["n2k_address"]    = n2k::sourceAddress();
    doc["n2k_tx_count"]   = n2k::txCount();
    doc["n2k_distressed"] = n2k::busDistressed();
    doc["ble_enabled"]    = s.ble_enabled;
    doc["ble_active"]     = ble::active();
    doc["ble_tx_count"]   = ble::txCount();
    doc["ble_mac"]        = ble::macAddress();
    doc["uptime_ms"]    = (uint32_t)millis();
    doc["ap_ssid"]      = wifimgr::apSsid();
    IPAddress ap = wifimgr::apIp();
    doc["ap_ip"]        = ap == IPAddress(0,0,0,0) ? String("") : ap.toString();
    doc["sta_state"]    = staStateName(wifimgr::staState());
    doc["sta_ssid"]     = wifimgr::staSsid();
    doc["sta_ip"]       = wifimgr::staIp();
    doc["sta_reason"]   = wifimgr::staReason();
    doc["wifi_configured"] = wifimgr::isConfigured();
    sendJson(req, doc);
}

void handleWifiScan(AsyncWebServerRequest* req) {
    JsonDocument doc;
    bool running = wifimgr::scanInProgress();
    auto results = wifimgr::scanResults();
    if (!running && results.empty()) {
        wifimgr::startScan();
        running = true;
    }
    doc["in_progress"] = running;
    JsonArray arr = doc["results"].to<JsonArray>();
    for (const auto& e : results) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]    = e.ssid;
        o["rssi"]    = e.rssi;
        o["secured"] = e.secured;
    }
    sendJson(req, doc);
}

void handleGetConfig(AsyncWebServerRequest* req) {
    const auto& s = cfg::get();
    JsonDocument doc;
    doc["tank_capacity_l"] = s.tank_capacity_l;
    doc["sensor_range_mm"] = s.sensor_range_mm;
    doc["modbus_slave"]    = s.modbus_slave;
    doc["modbus_baud"]     = s.modbus_baud;
    doc["modbus_parity"]   = s.modbus_parity;
    doc["modbus_fn"]       = s.modbus_fn;
    doc["modbus_reg"]      = s.modbus_reg;
    doc["modbus_scale"]    = s.modbus_scale;
    doc["poll_ms"]         = s.poll_ms;
    doc["can_enabled"]     = s.can_enabled;
    doc["ble_enabled"]     = s.ble_enabled;
    sendJson(req, doc);
}

void handleProbeStatus(AsyncWebServerRequest* req) {
    auto p = sensor::probeStatus();
    JsonDocument doc;
    doc["running"] = p.running;
    doc["phase"]   = p.phase;
    doc["step"]    = p.step;
    doc["total"]   = p.total;
    doc["current"] = p.current;
    JsonArray arr = doc["hits"].to<JsonArray>();
    for (const auto& h : p.hits) {
        JsonObject o = arr.add<JsonObject>();
        o["baud"]   = h.baud;
        o["parity"] = h.parity;
        o["slave"]  = h.slave;
        o["fn"]     = h.fn;
        o["reg"]    = h.reg;
        o["value"]  = h.value;
    }
    sendJson(req, doc);
}

// ---- POST helpers ----

using JsonHandler  = std::function<void(AsyncWebServerRequest*, JsonDocument&)>;
using AuthRequired = std::function<bool()>;

const AuthRequired AUTH_ALWAYS = []() { return true; };

constexpr size_t MAX_BODY_BYTES = 4096;

// Stored on req->_tempObject. AsyncWebServerRequest's destructor calls free()
// on _tempObject regardless of whether the request completed or was aborted,
// so a single malloc'd buffer (with a flexible array for the body) won't leak.
struct PostCtx {
    bool   responded;   // body cb already sent a 4xx (e.g. body too large)
    size_t total;
    char   body[];
};

void registerJsonPost(const char* path, JsonHandler h, AuthRequired authReq) {
    g_server.on(path, HTTP_POST,
        // Runs once after body is fully received (or immediately if no body).
        [authReq, h](AsyncWebServerRequest* req) {
            auto* ctx = static_cast<PostCtx*>(req->_tempObject);
            if (ctx && ctx->responded) return;
            if (authReq() && !auth::authenticate(req)) return;
            JsonDocument doc;
            if (ctx && ctx->total > 0) {
                DeserializationError err = deserializeJson(doc, ctx->body, ctx->total);
                if (err) {
                    req->send(400, "text/plain", "bad JSON");
                    return;
                }
            }
            h(req, doc);
        },
        nullptr,
        // Body chunks. Allocates a single PostCtx + body buffer at index==0.
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
              size_t index, size_t total) {
            if (index == 0) {
                if (total > MAX_BODY_BYTES) {
                    auto* ctx = static_cast<PostCtx*>(malloc(sizeof(PostCtx)));
                    if (ctx) { ctx->responded = true; ctx->total = 0; }
                    req->_tempObject = ctx;
                    req->send(413, "text/plain", "body too large");
                    return;
                }
                auto* ctx = static_cast<PostCtx*>(malloc(sizeof(PostCtx) + total));
                if (!ctx) {
                    req->send(500, "text/plain", "out of memory");
                    return;
                }
                ctx->responded = false;
                ctx->total     = total;
                req->_tempObject = ctx;
            }
            auto* ctx = static_cast<PostCtx*>(req->_tempObject);
            if (!ctx || ctx->responded) return;
            memcpy(ctx->body + index, data, len);
        });
}

void cfgFromJson(cfg::Settings& s, JsonDocument& doc) {
    if (doc["tank_capacity_l"].is<uint32_t>()) s.tank_capacity_l = doc["tank_capacity_l"];
    if (doc["sensor_range_mm"].is<uint16_t>()) s.sensor_range_mm = doc["sensor_range_mm"];
    if (doc["modbus_slave"].is<uint8_t>())     s.modbus_slave    = doc["modbus_slave"];
    if (doc["modbus_baud"].is<uint32_t>())     s.modbus_baud     = doc["modbus_baud"];
    if (doc["modbus_parity"].is<uint8_t>())    s.modbus_parity   = doc["modbus_parity"];
    if (doc["modbus_fn"].is<uint8_t>())        s.modbus_fn       = doc["modbus_fn"];
    if (doc["modbus_reg"].is<uint16_t>())      s.modbus_reg      = doc["modbus_reg"];
    if (doc["modbus_scale"].is<float>())       s.modbus_scale    = doc["modbus_scale"];
    if (doc["poll_ms"].is<uint16_t>())         s.poll_ms         = doc["poll_ms"];
    if (doc["can_enabled"].is<bool>())         s.can_enabled     = doc["can_enabled"];
    if (doc["ble_enabled"].is<bool>())         s.ble_enabled     = doc["ble_enabled"];
}

// Returns nullptr if valid, otherwise a static error message.
const char* validateSettings(const cfg::Settings& s) {
    if (s.tank_capacity_l < 1 || s.tank_capacity_l > 100000)
        return "tank_capacity_l out of range (1..100000)";
    if (s.sensor_range_mm < 50 || s.sensor_range_mm > 30000)
        return "sensor_range_mm out of range (50..30000)";
    if (s.modbus_slave < 1 || s.modbus_slave > 247)
        return "modbus_slave out of range (1..247)";
    static constexpr uint32_t valid_bauds[] = {
        1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
    };
    bool baud_ok = false;
    for (uint32_t b : valid_bauds) if (s.modbus_baud == b) { baud_ok = true; break; }
    if (!baud_ok) return "modbus_baud not supported";
    if (s.modbus_parity > 2)
        return "modbus_parity must be 0..2";
    if (s.modbus_fn != 0x03 && s.modbus_fn != 0x04)
        return "modbus_fn must be 0x03 or 0x04";
    if (!isfinite(s.modbus_scale) || s.modbus_scale <= 0.0f)
        return "modbus_scale must be > 0";
    if (s.poll_ms < 100 || s.poll_ms > 60000)
        return "poll_ms out of range (100..60000)";
    return nullptr;
}

}  // namespace

void begin() {
    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Stream from flash in chunks — avoids a ~57 KB heap copy that fails
        // when BLE stack has consumed most of the contiguous free heap.
        static constexpr size_t HTML_LEN = sizeof(INDEX_HTML) - 1;
        AsyncWebServerResponse* r = req->beginChunkedResponse("text/html",
            [](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
                if (index >= HTML_LEN) return 0;
                size_t n = min(maxLen, HTML_LEN - index);
                memcpy(buf, INDEX_HTML + index, n);
                return n;
            });
        r->addHeader("Cache-Control", "no-store");
        req->send(r);
    });

    g_server.on("/api/status",       HTTP_GET, handleStatus);
    g_server.on("/api/config",       HTTP_GET, handleGetConfig);
    g_server.on("/api/probe/status", HTTP_GET, handleProbeStatus);
    g_server.on("/api/wifi/scan",    HTTP_GET, handleWifiScan);

    registerJsonPost("/api/config", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        cfg::Settings s = cfg::get();
        cfgFromJson(s, doc);
        if (const char* err = validateSettings(s)) {
            JsonDocument resp;
            resp["ok"]    = false;
            resp["error"] = err;
            sendJson(req, resp, 400);
            return;
        }
        cfg::save(s);
        req->send(200, "application/json", "{\"ok\":true}");
    }, AUTH_ALWAYS);

    // /api/wifi: no auth on the very first WiFi save (bootstrap), auth required afterwards.
    registerJsonPost("/api/wifi", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        String ssid = doc["ssid"] | "";
        String pass = doc["pass"] | "";
        wifimgr::setStaCreds(ssid, pass);
        req->send(200, "application/json", "{\"ok\":true}");
    }, []() { return wifimgr::isConfigured(); });

    registerJsonPost("/api/probe", [](AsyncWebServerRequest* req, JsonDocument&) {
        sensor::startProbe();
        req->send(200, "application/json", "{\"ok\":true}");
    }, AUTH_ALWAYS);

    registerJsonPost("/api/probe/stop", [](AsyncWebServerRequest* req, JsonDocument&) {
        sensor::stopProbe();
        req->send(200, "application/json", "{\"ok\":true}");
    }, AUTH_ALWAYS);

    // No HTTP Basic Auth here — the old-password field IS the auth.
    // Otherwise the browser prompts twice.
    registerJsonPost("/api/admin/password", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        String oldPwd = doc["old"] | "";
        String newPwd = doc["new"] | "";
        if (auth::changePassword(oldPwd, newPwd)) {
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(400, "application/json", "{\"ok\":false}");
        }
    }, []() { return false; });

    // Restart helper: fires from a FreeRTOS timer so the response is fully
    // flushed before the reset. delay()+ESP.restart() inside an async callback
    // blocks the lwIP task and races the TCP send queue.
    auto scheduleRestart = [](uint32_t ms) {
        TimerHandle_t t = xTimerCreate("rst", pdMS_TO_TICKS(ms), pdFALSE,
                                       nullptr, [](TimerHandle_t h) {
            xTimerDelete(h, 0);
            ESP.restart();
        });
        if (t) xTimerStart(t, 0);
    };

    registerJsonPost("/api/restart", [scheduleRestart](AsyncWebServerRequest* req, JsonDocument&) {
        req->send(200, "application/json", "{\"ok\":true}");
        scheduleRestart(300);
    }, AUTH_ALWAYS);

    // OTA firmware upload. Multipart form with a single .bin file. Streams
    // chunks into the inactive OTA partition; on success, reboots into it.
    // Auth is checked at the first chunk; if it fails, auth::authenticate()
    // sends 401 inline and the rest of the body is discarded. ctx is malloc'd
    // so AsyncWebServerRequest's destructor (which frees _tempObject) cleans
    // it up however the request ends.
    //
    // Update.end() blocks for ~1–3 s while it verifies the SHA256 of the full
    // image. We run it inline on the final chunk (rather than handing off to a
    // FreeRTOS task) — the prior async-task design raced the request handler:
    // by the time the task tried req->send(), AsyncWebServer had already sent a
    // default 501 because the request handler returned without committing a
    // response. Inline blocking is safe here because OTA is the only thing in
    // flight; nothing else needs the async task during the final-chunk window.
    struct OtaCtx { bool authed; bool started; bool succeeded; char err[64]; };
    g_server.on("/api/ota", HTTP_POST,
        [scheduleRestart](AsyncWebServerRequest* req) {
            auto* ctx = static_cast<OtaCtx*>(req->_tempObject);
            if (!ctx || !ctx->authed) return;      // 401 already sent
            AsyncResponseStream* r = req->beginResponseStream("application/json");
            if (ctx->succeeded) {
                r->print("{\"ok\":true,\"msg\":\"rebooting\"}");
                scheduleRestart(500);              // reboot after response flushes
            } else {
                r->setCode(500);
                const char* errMsg = ctx->err[0] ? ctx->err : "upload incomplete";
                r->printf("{\"ok\":false,\"error\":\"%s\"}", errMsg);
            }
            r->addHeader("Connection", "close");
            req->send(r);
        },
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            auto* ctx = static_cast<OtaCtx*>(req->_tempObject);
            if (index == 0) {
                if (!ctx) {
                    ctx = static_cast<OtaCtx*>(calloc(1, sizeof(OtaCtx)));
                    req->_tempObject = ctx;
                }
                if (!ctx) return;
                if (!auth::authenticate(req)) return;   // 401 sent inline
                ctx->authed = true;
                Update.abort();
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    snprintf(ctx->err, sizeof(ctx->err), "begin: %s", Update.errorString());
                    Serial.printf("[ota] begin failed: %s\n", Update.errorString());
                    return;
                }
                ctx->started = true;
                Serial.printf("[ota] upload begin: %s (%u bytes)\n",
                              filename.c_str(), req->contentLength());
            }
            if (!ctx || !ctx->authed || !ctx->started) return;
            if (Update.write(data, len) != len) {
                snprintf(ctx->err, sizeof(ctx->err), "write: %s", Update.errorString());
                Update.printError(Serial);
            }
            if (final) {
                if (Update.end(true)) {
                    ctx->succeeded = true;
                    Serial.println("[ota] done, rebooting");
                } else {
                    snprintf(ctx->err, sizeof(ctx->err), "end: %s", Update.errorString());
                    Serial.printf("[ota] end failed: %s\n", Update.errorString());
                }
            }
        },
        nullptr);

    // --- Session auth routes ---

    // POST /api/login — body {password}; sets HttpOnly session cookie on success.
    registerJsonPost("/api/login", [](AsyncWebServerRequest* req, JsonDocument& doc) {
        String pwd = doc["password"] | "";
        if (!auth::loginSession(pwd)) {
            req->send(401, "application/json", "{\"ok\":false}");
            return;
        }
        // Build response with Set-Cookie header.
        String tok = auth::sessionToken();
        AsyncWebServerResponse* r = req->beginResponse(200, "application/json", "{\"ok\":true}");
        r->addHeader("Set-Cookie",
                     "session=" + tok + "; HttpOnly; SameSite=Strict; Path=/");
        req->send(r);
    }, []() { return false; });

    // POST /api/logout — clears the session cookie.
    registerJsonPost("/api/logout", [](AsyncWebServerRequest* req, JsonDocument&) {
        auth::logoutSession();
        AsyncWebServerResponse* r = req->beginResponse(200, "application/json", "{\"ok\":true}");
        r->addHeader("Set-Cookie", "session=; Max-Age=0; Path=/");
        req->send(r);
    }, []() { return false; });

    // GET /api/auth — lets the page check whether a session cookie is still valid.
    g_server.on("/api/auth", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["authed"] = auth::checkRequestSession(req);
        sendJson(req, doc);
    });

    // Captive-portal probe URLs from iOS / Android / Windows.
    // Redirect everything to the AP IP root so the device's web UI pops open.
    auto captiveRedirect = [](AsyncWebServerRequest* req) {
        String url = "http://" + wifimgr::apIp().toString() + "/";
        AsyncWebServerResponse* r = req->beginResponse(302, "text/plain", "");
        r->addHeader("Location", url);
        req->send(r);
    };
    g_server.on("/generate_204",      HTTP_GET, captiveRedirect);   // Android
    g_server.on("/gen_204",           HTTP_GET, captiveRedirect);   // Android (alt)
    g_server.on("/hotspot-detect.html", HTTP_GET, captiveRedirect); // iOS / macOS
    g_server.on("/library/test/success.html", HTTP_GET, captiveRedirect); // iOS (older)
    g_server.on("/connecttest.txt",   HTTP_GET, captiveRedirect);   // Windows
    g_server.on("/ncsi.txt",          HTTP_GET, captiveRedirect);   // Windows
    g_server.on("/redirect",          HTTP_GET, captiveRedirect);

    g_server.onNotFound([captiveRedirect](AsyncWebServerRequest* req) {
        // While in setup mode, treat unknown URLs as captive probes too.
        if (!wifimgr::isConfigured() && req->method() == HTTP_GET) {
            captiveRedirect(req);
            return;
        }
        req->send(404, "text/plain", "not found");
    });

    g_server.begin();
}

}  // namespace webui
