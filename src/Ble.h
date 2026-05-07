#pragma once
#include <Arduino.h>

namespace ble {

void begin();
void loop();
void publishLevel(float level_mm);  // physical depth in mm; pre-divided by Mopeka 0.704 factor internally

bool     active();
uint32_t txCount();
String   macAddress();  // returns MAC when active, empty string otherwise

}  // namespace ble
