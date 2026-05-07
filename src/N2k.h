#pragma once

#include <Arduino.h>

namespace n2k {

void begin();
void loop();                         // pump ParseMessages, send if due
void publishLevel(float pct, uint32_t capacity_l);   // cache for next send

uint8_t  sourceAddress();            // 254 = unclaimed
uint32_t txCount();
bool     ready();                    // address claimed, capacity non-zero, value cached
bool     busDistressed();            // TWAI controller in non-running state or error-passive

}  // namespace n2k
