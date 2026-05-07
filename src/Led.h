#pragma once

namespace led {

enum class State {
    Booting,
    ApOnly,        // STA not connected, no sensor yet
    SensorErr,
    SensorOk,      // sensor reading good, n2k not yet claimed
    SensorOnly,    // sensor reading good, CAN and BLE both disabled
    BleOnly,       // sensor reading good, CAN disabled, BLE advertising
    Publishing,    // sensor good and n2k publishing
    Probing,
};

void begin();
void set(State s);
void loop();       // call regularly to handle blink animations

}  // namespace led
