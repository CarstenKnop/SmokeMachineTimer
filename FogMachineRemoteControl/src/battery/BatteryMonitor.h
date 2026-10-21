// BatteryMonitor.h - LiPo voltage through the 2x200k divider on A0, plus charger status pins.
#pragma once

#include <Arduino.h>
#include "config/RemoteSettings.h"

class BatteryMonitor {
public:
    BatteryMonitor(uint8_t adcPin, RemoteSettings& settings) : pin_(adcPin), settings_(settings) {}

    void begin();
    // Takes a fresh oversampled reading about once per second; cheap to call every loop.
    void update(uint32_t nowMs);
    uint16_t millivolts() const { return mv_; }
    uint8_t percent() const;
    bool charging() const;
    bool usbPower() const;
    uint16_t readMillivoltsNow() const;      // immediate reading (calibration screen)

private:
    uint8_t pin_;
    RemoteSettings& settings_;
    uint16_t mv_ = 0;
    uint32_t lastMs_ = 0;
};
