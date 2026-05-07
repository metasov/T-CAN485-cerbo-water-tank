#include "Auth.h"
#include "Config.h"

#include <ESPAsyncWebServer.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

namespace auth {

namespace {
String g_bootstrap_password;
char   g_session_token[65];  // 32-byte hex, empty = no active session

void hash(const uint8_t salt[cfg::PWD_SALT_LEN], const String& pwd,
          uint8_t out[cfg::PWD_HASH_LEN]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, cfg::PWD_SALT_LEN);
    mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(pwd.c_str()),
                          pwd.length());
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

bool eqConstTime(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

String randomPassword(size_t n) {
    static const char* alphabet =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    const size_t alen = strlen(alphabet);
    String s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) s += alphabet[esp_random() % alen];
    return s;
}

void mintSessionToken() {
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        uint8_t b = esp_random() & 0xFF;
        g_session_token[i * 2]     = hex[b >> 4];
        g_session_token[i * 2 + 1] = hex[b & 0xF];
    }
    g_session_token[64] = '\0';
}
}  // namespace

void begin() {
    cfg::Settings s = cfg::get();

    // Regenerate the admin password on EVERY boot until the user has changed
    // it via the web UI. This way the user can always recover by rebooting
    // with the serial monitor attached.
    if (s.pwd_set && !s.pwd_is_default) return;

    for (size_t i = 0; i < cfg::PWD_SALT_LEN; ++i) s.pwd_salt[i] = esp_random() & 0xFF;
    g_bootstrap_password = randomPassword(8);
    hash(s.pwd_salt, g_bootstrap_password, s.pwd_hash);
    s.pwd_set = true;
    s.pwd_is_default = true;
    cfg::save(s);

    // Print the password 3× with delays so it survives a serial-monitor
    // attach race after `pio run -t upload && pio device monitor`.
    for (int i = 0; i < 3; ++i) {
        Serial.println();
        Serial.println(F("================================================"));
        Serial.print  (F("  Admin user: admin   Password: "));
        Serial.println(g_bootstrap_password);
        Serial.println(F("  This regenerates on every boot until you"));
        Serial.println(F("  change it from the web UI."));
        Serial.println(F("================================================"));
        Serial.println();
        delay(500);
    }
}

bool check(const String& password) {
    const cfg::Settings& s = cfg::get();
    if (!s.pwd_set) return false;
    uint8_t h[cfg::PWD_HASH_LEN];
    hash(s.pwd_salt, password, h);
    return eqConstTime(h, s.pwd_hash, cfg::PWD_HASH_LEN);
}

bool changePassword(const String& oldPwd, const String& newPwd) {
    if (!check(oldPwd)) return false;
    if (newPwd.length() < 4) return false;
    cfg::Settings s = cfg::get();
    for (size_t i = 0; i < cfg::PWD_SALT_LEN; ++i) s.pwd_salt[i] = esp_random() & 0xFF;
    hash(s.pwd_salt, newPwd, s.pwd_hash);
    s.pwd_set = true;
    s.pwd_is_default = false;       // stop regenerating on every boot
    cfg::save(s);
    g_bootstrap_password = "";
    return true;
}

static bool checkBasic(AsyncWebServerRequest* req) {
    if (!req->hasHeader("Authorization")) return false;
    String h = req->getHeader("Authorization")->value();
    if (!h.startsWith("Basic ")) return false;
    String b64 = h.substring(6);
    size_t out_len = 0;
    uint8_t buf[96];
    if (mbedtls_base64_decode(buf, sizeof(buf), &out_len,
                              reinterpret_cast<const uint8_t*>(b64.c_str()),
                              b64.length()) != 0) return false;
    buf[out_len] = 0;
    char* colon = strchr(reinterpret_cast<char*>(buf), ':');
    if (!colon) return false;
    *colon = 0;
    return strcmp(reinterpret_cast<char*>(buf), USER) == 0 && check(String(colon + 1));
}

bool checkRequestSession(AsyncWebServerRequest* req) {
    if (!req->hasHeader("Cookie")) return false;
    String cookies = req->getHeader("Cookie")->value();
    int idx = cookies.indexOf("session=");
    if (idx < 0) return false;
    String tok = cookies.substring(idx + 8);
    int end = tok.indexOf(';');
    if (end >= 0) tok = tok.substring(0, end);
    tok.trim();
    return checkSession(tok);
}

bool authenticate(AsyncWebServerRequest* req) {
    if (checkRequestSession(req)) return true;       // cookie session (JS flow)
    if (checkBasic(req))          return true;       // HTTP Basic fallback (curl)

    // No WWW-Authenticate — suppresses browser's native dialog so JS handles 401.
    req->send(401, "text/plain", "Unauthorized");
    return false;
}

String currentBootstrapPassword() { return g_bootstrap_password; }

bool loginSession(const String& password) {
    if (!check(password)) return false;
    mintSessionToken();
    return true;
}

bool checkSession(const String& token) {
    if (g_session_token[0] == '\0') return false;
    if (token.length() != 64) return false;
    // Constant-time compare
    uint8_t diff = 0;
    for (int i = 0; i < 64; ++i) diff |= token[i] ^ g_session_token[i];
    return diff == 0;
}

void logoutSession() {
    memset(g_session_token, 0, sizeof(g_session_token));
}

const char* sessionToken() { return g_session_token; }

}  // namespace auth
