#include "Sensor.h"
#include "Config.h"
#include "pin_config.h"

#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdarg.h>

namespace sensor {

namespace {

HardwareSerial& RS = Serial1;

Reading      g_last;
uint32_t     g_ok = 0, g_err = 0;
uint32_t     g_next_poll_ms = 0;

ProbeStatus  g_probe;
SemaphoreHandle_t g_mu;
TaskHandle_t g_probe_task = nullptr;
volatile bool g_probe_stop = false;

uint32_t  g_active_baud   = 0;
uint8_t   g_active_parity = 0xFF;

uint32_t serialConfigFor(uint8_t parity) {
    switch (parity) {
        case 1: return SERIAL_8E1;
        case 2: return SERIAL_8O1;
        default: return SERIAL_8N1;
    }
}

void applyUart(uint32_t baud, uint8_t parity) {
    if (baud == g_active_baud && parity == g_active_parity) return;
    RS.flush();
    RS.end();
    RS.begin(baud, serialConfigFor(parity), RS485_RX, RS485_TX);
    g_active_baud   = baud;
    g_active_parity = parity;
    delay(5);
}

uint16_t modbusCrc(const uint8_t* buf, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

void drainRx() {
    while (RS.available()) RS.read();
}

// Send a 0x03/0x04 read request for `qty` registers; expect response into rsp[].
// Returns number of payload bytes copied (qty*2) on success, 0 on error.
size_t modbusRead(uint8_t slave, uint8_t fn, uint16_t reg, uint16_t qty,
                  uint8_t* rsp, size_t rsp_cap, uint32_t timeout_ms) {
    if (rsp_cap < (size_t)(qty * 2)) return 0;

    uint8_t req[8];
    req[0] = slave;
    req[1] = fn;
    req[2] = reg >> 8;
    req[3] = reg & 0xFF;
    req[4] = qty >> 8;
    req[5] = qty & 0xFF;
    uint16_t crc = modbusCrc(req, 6);
    req[6] = crc & 0xFF;
    req[7] = crc >> 8;

    drainRx();
    RS.write(req, 8);
    RS.flush();  // wait for transmit complete (AutoDirection on MAX13487)

    // Expected response: slave, fn, byteCount, payload..., crc_lo, crc_hi
    const size_t expected = 5 + qty * 2;
    uint8_t buf[64];
    if (expected > sizeof(buf)) return 0;

    size_t got = 0;
    uint32_t deadline = millis() + timeout_ms;
    while (got < expected && (int32_t)(deadline - millis()) > 0) {
        if (RS.available()) {
            buf[got++] = RS.read();
        } else {
            vTaskDelay(1);
        }
    }
    if (got < expected) return 0;
    if (buf[0] != slave || buf[1] != fn || buf[2] != qty * 2) return 0;
    uint16_t got_crc = buf[expected - 2] | (buf[expected - 1] << 8);
    if (got_crc != modbusCrc(buf, expected - 2)) return 0;

    memcpy(rsp, &buf[3], qty * 2);
    return qty * 2;
}

// Mutex-guarded progress updates used by probeTask. All three phases share
// the same scaffold: bump step, format current, give. setPhase resets step.
void setPhase(int phase, int total, const char* label) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_probe.phase   = phase;
    g_probe.step    = 0;
    g_probe.total   = total;
    g_probe.current = label;
    xSemaphoreGive(g_mu);
}

void tickProgress(const char* fmt, ...) {
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    xSemaphoreTake(g_mu, portMAX_DELAY);
    ++g_probe.step;
    g_probe.current = buf;
    xSemaphoreGive(g_mu);
}

// Same but only returns true if the framing is plausible (any bytes at all,
// and first byte equals slave). Used by probe phase A.
bool modbusAnyResponse(uint8_t slave, uint8_t fn, uint16_t reg, uint32_t timeout_ms) {
    uint8_t req[8] = {slave, fn, (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), 0, 1, 0, 0};
    uint16_t crc = modbusCrc(req, 6);
    req[6] = crc & 0xFF;
    req[7] = crc >> 8;
    drainRx();
    RS.write(req, 8);
    RS.flush();
    uint32_t deadline = millis() + timeout_ms;
    while ((int32_t)(deadline - millis()) > 0) {
        if (RS.available()) return true;
        vTaskDelay(1);
    }
    return false;
}

void doSinglePoll() {
    const cfg::Settings& s = cfg::get();
    applyUart(s.modbus_baud, s.modbus_parity);

    uint8_t payload[2] = {0, 0};
    size_t got = modbusRead(s.modbus_slave, s.modbus_fn, s.modbus_reg, 1,
                            payload, sizeof(payload), 200);
    Reading r;
    r.ts_ms = millis();
    if (got == 2) {
        r.raw       = (uint32_t)payload[0] << 8 | payload[1];
        r.level_mm  = (float)r.raw * s.modbus_scale;
        float pct = 100.0f * r.level_mm / (float)s.sensor_range_mm;
        r.level_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
        r.valid     = true;
        ++g_ok;
    } else {
        r.last_error = "no/invalid Modbus response";
        ++g_err;
    }
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_last = r;
    xSemaphoreGive(g_mu);
}

void probeTask(void*) {
    constexpr uint32_t bauds[]    = {9600, 19200, 4800, 2400, 1200};
    constexpr uint8_t  parities[] = {0, 1, 2};
    constexpr uint16_t regs[]     = {0x0000, 0x0001, 0x0002, 0x0004};
    constexpr uint8_t  fns[]      = {0x03, 0x04};
    struct Candidate { uint32_t baud; uint8_t parity; uint8_t slave; };

    std::vector<std::pair<uint32_t,uint8_t>> lanes;
    std::vector<Candidate>                   candidates;

    // ── Phase 1: baud/parity lane discovery (50 ms each) ──────────────────
    setPhase(1, sizeof(bauds)/sizeof(bauds[0]) * sizeof(parities)/sizeof(parities[0]),
             "Lane discovery");
    for (uint32_t b : bauds) {
        for (uint8_t p : parities) {
            if (g_probe_stop) goto done;
            applyUart(b, p);
            if (modbusAnyResponse(1, 0x03, 0x0000, 50))
                lanes.push_back({b, p});
            tickProgress("Lane %lu / parity %u", (unsigned long)b, p);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (lanes.empty())
        for (uint32_t b : bauds) for (uint8_t p : parities)
            lanes.push_back({b, p});

    // ── Phase 2: slave address scan (50 ms each) ──────────────────────────
    setPhase(2, (int)lanes.size() * 16, "Slave scan");
    for (auto& lane : lanes) {
        applyUart(lane.first, lane.second);
        for (uint8_t addr = 1; addr <= 16; ++addr) {
            if (g_probe_stop) goto done;
            if (modbusAnyResponse(addr, 0x03, 0x0000, 50))
                candidates.push_back({lane.first, lane.second, addr});
            tickProgress("Slave %u @ %lu/%u",
                         addr, (unsigned long)lane.first, lane.second);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    // No responding slave found — fallback to addr 1–4 only
    if (candidates.empty())
        for (auto& lane : lanes)
            for (uint8_t addr = 1; addr <= 4; ++addr)
                candidates.push_back({lane.first, lane.second, addr});

    // ── Phase 3: register sweep (80 ms each) ──────────────────────────────
    setPhase(3, (int)candidates.size()
                * (int)(sizeof(regs)/sizeof(regs[0]))
                * (int)(sizeof(fns)/sizeof(fns[0])),
             "Register sweep");
    for (auto& c : candidates) {
        applyUart(c.baud, c.parity);
        for (uint8_t fn : fns) {
            for (uint16_t reg : regs) {
                if (g_probe_stop) goto done;
                uint8_t pay[2] = {0, 0};
                size_t got = modbusRead(c.slave, fn, reg, 1, pay, sizeof(pay), 80);
                if (got == 2) {
                    ProbeHit h{c.baud, c.parity, c.slave, fn, reg,
                               (uint16_t)((uint16_t)pay[0] << 8 | pay[1])};
                    xSemaphoreTake(g_mu, portMAX_DELAY);
                    g_probe.hits.push_back(h);
                    xSemaphoreGive(g_mu);
                }
                tickProgress("%lu/%u  addr=%u fn=0x%02X reg=0x%04X",
                             (unsigned long)c.baud, c.parity, c.slave, fn, reg);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
    }

done:
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_probe.phase = 0;
    g_probe.running = false;
    g_probe.current = g_probe_stop ? "Cancelled" : "Done";
    xSemaphoreGive(g_mu);

    // Restore configured UART
    const cfg::Settings& s = cfg::get();
    applyUart(s.modbus_baud, s.modbus_parity);

    g_probe_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void begin() {
    g_mu = xSemaphoreCreateMutex();
    const cfg::Settings& s = cfg::get();
    applyUart(s.modbus_baud, s.modbus_parity);
}

void poll() {
    if (g_probe_task) return;  // probe owns the bus
    uint32_t now = millis();
    if ((int32_t)(now - g_next_poll_ms) < 0) return;
    g_next_poll_ms = now + cfg::get().poll_ms;
    doSinglePoll();
}

Reading last() {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    Reading snap = g_last;
    xSemaphoreGive(g_mu);
    return snap;
}
uint32_t okCount() { return g_ok; }
uint32_t errCount() { return g_err; }

void startProbe() {
    if (g_probe_task) return;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_probe = ProbeStatus{};
    g_probe.running = true;
    g_probe_stop = false;
    xSemaphoreGive(g_mu);
    xTaskCreatePinnedToCore(probeTask, "modbus_probe", 4096, nullptr, 1, &g_probe_task, 1);
}

void stopProbe() { g_probe_stop = true; }

ProbeStatus probeStatus() {
    xSemaphoreTake(g_mu, portMAX_DELAY);
    ProbeStatus snap = g_probe;
    xSemaphoreGive(g_mu);
    return snap;
}

}  // namespace sensor
