// CalibrationManager.cpp
// Handles battery calibration logic and persistence.
#include "CalibrationManager.h"
#include <EEPROM.h>

CalibrationManager::CalibrationManager() {
    // Defaults tuned for a simple divider targeting ~0-100% mapping on 12-bit ADC
    // Adjust if your divider changes; these are conservative starting points.
    calibAdc[0] = 1900;  // ~0%
    calibAdc[1] = 2600;  // ~50%
    calibAdc[2] = 3200;  // ~100%
}

void CalibrationManager::begin() {
    loadFromEEPROM();
}

void CalibrationManager::loadFromEEPROM() {
    uint16_t buf[3] = {0,0,0};
    EEPROM.get(64, buf);
    // If EEPROM is fresh/zeros, keep defaults.
    if (buf[0] != 0 || buf[1] != 0 || buf[2] != 0) {
        calibAdc[0] = buf[0]; calibAdc[1] = buf[1]; calibAdc[2] = buf[2];
    }
}

void CalibrationManager::saveToEEPROM() {
    EEPROM.put(64, calibAdc);
    EEPROM.commit();
}

void CalibrationManager::setCalibrationPoints(uint16_t adc0, uint16_t adc50, uint16_t adc100) {
    calibAdc[0] = adc0; calibAdc[1] = adc50; calibAdc[2] = adc100;
    saveToEEPROM();
}

void CalibrationManager::getCalibrationPoints(uint16_t& adc0, uint16_t& adc50, uint16_t& adc100) const {
    adc0 = calibAdc[0]; adc50 = calibAdc[1]; adc100 = calibAdc[2];
}

uint8_t CalibrationManager::calculatePercent(uint16_t adcValue) const {
    if (adcValue <= calibAdc[0]) return 0;
    if (adcValue >= calibAdc[2]) return 100;
    if (adcValue <= calibAdc[1]) {
        return (uint8_t)(((adcValue - calibAdc[0]) * 50) / (calibAdc[1] - calibAdc[0]));
    }
    return (uint8_t)(50 + ((adcValue - calibAdc[1]) * 50) / (calibAdc[2] - calibAdc[1]));
}

void CalibrationManager::resetToDefaults() {
    // Match constructor defaults
    calibAdc[0] = 1900;
    calibAdc[1] = 2600;
    calibAdc[2] = 3200;
    saveToEEPROM();
}
