// CalibrationManager.cpp
// Handles battery calibration logic and persistence.
#include "CalibrationManager.h"
#include <EEPROM.h>

CalibrationManager::CalibrationManager() {
    calibAdc[0] = 2000; calibAdc[1] = 3000; calibAdc[2] = 3500;
}

void CalibrationManager::begin() {
    loadFromEEPROM();
}

void CalibrationManager::loadFromEEPROM() {
    EEPROM.get(64, calibAdc);
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
