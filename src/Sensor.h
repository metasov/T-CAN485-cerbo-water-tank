#pragma once

#include <Arduino.h>
#include <vector>

namespace sensor {

struct Reading {
    bool        valid      = false;
    uint32_t    raw        = 0;
    float       level_mm   = 0.0f;
    float       level_pct  = 0.0f;     // 0..100, clamped to sensor_range
    uint32_t    ts_ms      = 0;
    const char* last_error = nullptr;  // string literal or nullptr
};

struct ProbeHit {
    uint32_t baud;
    uint8_t  parity;        // 0=N, 1=E, 2=O
    uint8_t  slave;
    uint8_t  fn;            // 0x03 / 0x04
    uint16_t reg;
    uint16_t value;
};

struct ProbeStatus {
    bool     running = false;
    uint8_t  phase = 0;     // 0 idle, 1 lane discovery, 2 address sweep, 3 done
    uint16_t step  = 0;
    uint16_t total = 0;
    String   current;       // human description
    std::vector<ProbeHit> hits;
};

void begin();
void poll();                     // call from main loop
Reading last();                  // returns a snapshot
uint32_t okCount();
uint32_t errCount();

void startProbe();
void stopProbe();
ProbeStatus probeStatus();       // returns a snapshot

}  // namespace sensor
