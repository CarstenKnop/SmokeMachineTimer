#pragma once
#include <Arduino.h>
#include <EEPROM.h>
#include "Defaults.h"

class Config {
public:
  struct Values {
    uint32_t offTime = 100; // tenths
    uint32_t onTime  = 100; // tenths
    char deviceName[24] = "FogTimer";
    uint16_t calibAdc[3] = {0,0,0}; // raw ADC values for 3-point battery calibration
  };

  bool begin(size_t eepromSize = 128) {
    EEPROM.begin(eepromSize);
    load();
    return true;
  }
  void load() {
    EEPROM.get(0, vals.offTime);
    EEPROM.get(sizeof(uint32_t), vals.onTime);
    EEPROM.get(sizeof(uint32_t)*2, vals.deviceName);
    EEPROM.get(sizeof(uint32_t)*2 + sizeof(vals.deviceName), vals.calibAdc);
    if (vals.offTime < TIMER_MIN || vals.offTime > TIMER_MAX) vals.offTime = 100;
    if (vals.onTime  < TIMER_MIN || vals.onTime  > TIMER_MAX) vals.onTime = 100;
  }
  void saveTimersIfChanged(uint32_t off, uint32_t on, bool changed) {
    if (!changed) return;
    vals.offTime = off; vals.onTime = on;
    EEPROM.put(0, vals.offTime);
    EEPROM.put(sizeof(uint32_t), vals.onTime);
    EEPROM.put(sizeof(uint32_t)*2 + sizeof(vals.deviceName), vals.calibAdc);
    EEPROM.commit();
  }
  void saveName(const char* name) {
    strncpy(vals.deviceName, name, sizeof(vals.deviceName)-1); vals.deviceName[sizeof(vals.deviceName)-1]=0;
    EEPROM.put(sizeof(uint32_t)*2, vals.deviceName);
    EEPROM.commit();
  }
  void saveCalibration(const uint16_t calib[3]) {
    memcpy(vals.calibAdc, calib, sizeof(vals.calibAdc));
    EEPROM.put(sizeof(uint32_t)*2 + sizeof(vals.deviceName), vals.calibAdc);
    EEPROM.commit();
  }
  Values& get() { return vals; }

private:
  Values vals;
};
