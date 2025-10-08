// CalibrationManager.h
// Handles battery calibration logic and persistence.
#pragma once
#include <Arduino.h>

class CalibrationManager {
public:
    CalibrationManager();
    void begin();
    void loadFromEEPROM();
    void saveToEEPROM();
    void setCalibrationPoints(uint16_t adc0, uint16_t adc50, uint16_t adc100);
    void getCalibrationPoints(uint16_t& adc0, uint16_t& adc50, uint16_t& adc100) const;
    uint8_t calculatePercent(uint16_t adcValue) const;
private:
    uint16_t calibAdc[3]; // 0%, 50%, 100%
};
