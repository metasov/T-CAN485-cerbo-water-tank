#pragma once

#include <Arduino.h>

class AsyncWebServerRequest;

namespace auth {

constexpr const char* USER = "admin";

void begin();                                 // bootstrap random password if unset
bool check(const String& password);
bool changePassword(const String& oldPwd, const String& newPwd);
bool authenticate(AsyncWebServerRequest* req);   // sends 401 if not authed (no WWW-Authenticate)
String currentBootstrapPassword();               // empty after first boot

// Cookie-session API (RAM-only; clears on reboot)
bool        loginSession(const String& password);   // check pwd, mint session token
bool        checkSession(const String& token);      // constant-time compare
bool        checkRequestSession(AsyncWebServerRequest* req);  // parse Cookie header, validate token
void        logoutSession();                        // clear token
const char* sessionToken();                         // current token ("" when logged out)

}  // namespace auth
