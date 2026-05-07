#include "Led.h"
#include "pin_config.h"
#include <FastLED.h>

namespace led {

namespace {
CRGB g_leds[1];
State g_state = State::Booting;
}

void begin() {
    FastLED.addLeds<WS2812B, WS2812B_DATA, GRB>(g_leds, 1);
    FastLED.setBrightness(20);
    g_leds[0] = CRGB::White;
    FastLED.show();
}

void set(State s) { g_state = s; }

void loop() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 100) return;
    last = now;

    bool blink_phase = ((now / 500) & 1) == 0;
    CRGB c;
    switch (g_state) {
        case State::Booting:    c = CRGB::White; break;
        case State::ApOnly:     c = CRGB::Purple; break;
        case State::SensorErr:  c = CRGB::Red; break;
        case State::SensorOk:   c = CRGB::Yellow; break;
        case State::SensorOnly: c = CRGB::Cyan;  break;
        case State::BleOnly:    c = CRGB::Blue;  break;
        case State::Publishing: c = CRGB::Green; break;
        case State::Probing:    c = blink_phase ? CRGB::Blue : CRGB::Black; break;
    }
    if (g_leds[0] != c) {
        g_leds[0] = c;
        FastLED.show();
    }
}

}  // namespace led
