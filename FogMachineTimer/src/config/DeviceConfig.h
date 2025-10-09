// DeviceConfig.h
// Handles persistent storage of timer values and device name in EEPROM.
#pragma once
#include <Arduino.h>
#include <EEPROM.h>

class DeviceConfig {
public:
    DeviceConfig();
    void begin(size_t eepromSize = 128);
    void load();
    void saveTimer(float ton, float toff);
    void saveName(const char* name);
    void factoryReset();
    bool isUninitialized() const;
    float getTon() const;
    float getToff() const;
    const char* getName() const;
    float ton, toff;
    char name[16];
         static constexpr uint8_t EEPROM_MAGIC = 0x42;
         static constexpr int EEPROM_MAGIC_ADDR = 100;
};
