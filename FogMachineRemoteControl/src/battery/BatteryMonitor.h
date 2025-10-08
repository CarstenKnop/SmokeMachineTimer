// BatteryMonitor.h
// Reads battery voltage via ADC and calculates percentage using calibration.
#pragma once
#include <Arduino.h>
#include "calibration/CalibrationManager.h"

class BatteryMonitor {
public:
    BatteryMonitor(uint8_t adcPin, CalibrationManager& calibMgr);
    void begin();
    uint16_t readRawAdc();
    uint8_t getPercent() const;
    float getVoltage();
private:
    uint8_t pin;
    CalibrationManager& calibration;
};
