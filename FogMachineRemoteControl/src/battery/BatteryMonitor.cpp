// BatteryMonitor.cpp
// Reads battery voltage via ADC and calculates percentage using calibration.
#include "BatteryMonitor.h"

BatteryMonitor::BatteryMonitor(uint8_t adcPin, CalibrationManager& calibMgr)
    : pin(adcPin), calibration(calibMgr) {}

void BatteryMonitor::begin() {
    pinMode(pin, INPUT);
}

uint16_t BatteryMonitor::readRawAdc() {
    return analogRead(pin);
}

uint8_t BatteryMonitor::getPercent() const {
    uint16_t raw = analogRead(pin);
    return calibration.calculatePercent(raw);
}

float BatteryMonitor::getVoltage() {
    uint16_t raw = readRawAdc();
    return raw * (3.3f / 4095.0f) * 2.0f; // Example for divider
}
