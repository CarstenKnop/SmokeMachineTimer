// BatteryMonitor.cpp
// Reads battery voltage via ADC and calculates percentage using calibration.
#include "BatteryMonitor.h"

BatteryMonitor::BatteryMonitor(uint8_t adcPin, CalibrationManager& calibMgr)
    : pin(adcPin), calibration(calibMgr) {}

void BatteryMonitor::begin() {
    pinMode(pin, INPUT);
}

uint16_t BatteryMonitor::readRawAdc() const {
    return analogRead(pin);
}

uint16_t BatteryMonitor::sampleAveraged() const {
    // Oversample to reduce instantaneous noise
    uint32_t acc = 0;
    for (uint8_t i = 0; i < OVERSAMPLE; ++i) {
        acc += analogRead(pin);
    }
    return (uint16_t)(acc / OVERSAMPLE);
}

uint8_t BatteryMonitor::getPercent() const {
    uint16_t raw = sampleAveraged();
    return calibration.calculatePercent(raw);
}

float BatteryMonitor::getVoltage() {
    uint16_t raw = sampleAveraged();
    const float vref = 3.3f;
    const float divider = 2.0f; // If using 1:1 divider to measure up to ~2x Vref
    return raw * (vref / 4095.0f) * divider;
}
