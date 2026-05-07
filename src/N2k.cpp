#include "N2k.h"
#include "Config.h"

#include <NMEA2000_esp32_twai.h>
#include <N2kMessages.h>
#include <esp_mac.h>

// CLAUDE.md says not to include driver/twai.h from app code because the lib
// owns the controller. We make a narrow exception here for two read/clear ops
// only — twai_get_status_info() (read-only) and twai_clear_transmit_queue()
// (drops pending TX frames). Lifecycle calls (install, start, stop, transmit)
// stay owned by the lib. Reason: on a disconnected bus the TX queue (depth 5)
// fills with frames that can't get an ACK; subsequent twai_transmit() calls
// fail and the fork logs ESP_LOGI once per failure, producing ~10–75 lines/s
// of noise. The lib's CtrlTask recovers bus-off cleanly but never drains the
// stuck queue, so we drain it from here.
#include <driver/twai.h>

// Global NMEA2000 instance — pins set via ESP32_CAN_TX_PIN/RX_PIN build flags.
NMEA2000_esp32_twai NMEA2000;

namespace n2k {

namespace {
constexpr uint16_t SEND_INTERVAL_MS  = 2500;
constexpr uint32_t LEVEL_STALE_MS    = 10000;
constexpr uint8_t  ADDR_UNCLAIMED    = 254;

// When the bus is in distress, throttle ParseMessages() and drain the stuck
// TX queue. tx_error_counter ≥ 96 is the TWAI hardware's "error-passive"
// threshold — a reliable signal that no peer is ACKing our frames.
constexpr uint32_t BACKOFF_PUMP_MS   = 5000;
constexpr uint8_t  TEC_DISTRESS      = 96;

bool     g_disabled       = false;
float    g_level_pct      = 0.0f;
uint32_t g_capacity_l     = 0;
bool     g_have_value     = false;
uint32_t g_value_ts       = 0;
uint32_t g_next_send      = 0;
uint32_t g_tx_count       = 0;
uint32_t g_last_pump      = 0;
bool     g_bus_distressed = false;
}  // namespace

void begin() {
    if (!cfg::get().can_enabled) { g_disabled = true; return; }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t uniqueId =
        ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
        ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    uniqueId &= 0x1FFFFF;  // NMEA 2000 unique-id is 21 bits

    NMEA2000.SetProductInformation(
        "TCAN485-001",     // model serial
        100,               // product code
        "T-CAN485 Tank",   // model id
        "1.0",             // sw version
        "T-CAN485"         // model version
    );
    NMEA2000.SetDeviceInformation(
        uniqueId,
        150,   // device function: Fluid Level
        75,    // device class: Sensor Communication Interface (Sensors)
        2046   // generic / unassigned manufacturer code
    );

    NMEA2000.SetMode(tNMEA2000::N2km_NodeOnly, 22);
    NMEA2000.EnableForward(false);
    NMEA2000.Open();
}

void publishLevel(float pct, uint32_t capacity_l) {
    if (g_disabled) return;
    g_level_pct  = constrain(pct, 0.0f, 100.0f);
    g_capacity_l = capacity_l;
    g_have_value = true;
    g_value_ts   = millis();
}

uint8_t  sourceAddress() { return g_disabled ? ADDR_UNCLAIMED : NMEA2000.GetN2kSource(); }
uint32_t txCount()       { return g_tx_count; }
bool     busDistressed() { return !g_disabled && g_bus_distressed; }
bool     ready() {
    if (g_disabled) return false;
    return NMEA2000.GetN2kSource() != ADDR_UNCLAIMED
        && g_capacity_l > 0
        && g_have_value;
}

void loop() {
    if (g_disabled) return;
    uint32_t now = millis();

    // Probe the TWAI controller. The fork's CtrlTask handles bus-off recovery
    // on its own, but it never drains the TX queue, so when no peer is on the
    // wire the queue (depth 5) fills with frames that can't be ACKed and every
    // subsequent send logs "Failed to queue message for transmission".
    twai_status_info_t st = {};
    bool got_status = (twai_get_status_info(&st) == ESP_OK);
    bool distressed = got_status &&
        (st.state != TWAI_STATE_RUNNING || st.tx_error_counter >= TEC_DISTRESS);

    if (distressed && st.state == TWAI_STATE_RUNNING) {
        // Drop the backed-up TX so the next pump doesn't trip queue-full again.
        // (clear is rejected unless RUNNING — bus-off / recovering states are
        // handled by the lib's CtrlTask which will eventually return to RUNNING.)
        twai_clear_transmit_queue();
    }
    g_bus_distressed = distressed;

    // Throttle the pump while distressed so we generate at most one log line
    // (when twai_transmit fails on the first frame after each clear) per cycle.
    if (distressed && (now - g_last_pump) < BACKOFF_PUMP_MS) return;
    g_last_pump = now;

    NMEA2000.ParseMessages();

    if (!ready()) return;
    if ((int32_t)(now - g_next_send) < 0) return;
    g_next_send = now + SEND_INTERVAL_MS;

    // If the cached level is older than LEVEL_STALE_MS the sensor has gone
    // quiet — keep the PGN flowing but mark the level as N/A so the Cerbo
    // shows the tank as offline rather than a frozen-but-valid value.
    bool stale = (uint32_t)(now - g_value_ts) > LEVEL_STALE_MS;

    tN2kMsg msg;
    SetN2kFluidLevel(msg, /*instance=*/0, N2kft_Water,
                     stale ? N2kDoubleNA : (double)g_level_pct,
                     (double)g_capacity_l);
    if (NMEA2000.SendMsg(msg)) ++g_tx_count;
}

}  // namespace n2k
