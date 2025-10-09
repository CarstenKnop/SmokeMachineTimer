// DeviceConfig.cpp
// Handles persistent storage of timer values and device name in EEPROM.
#include "DeviceConfig.h"

DeviceConfig::DeviceConfig() : ton(0.1f), toff(10.0f) {
    name[0] = '\0';
}

void DeviceConfig::begin(size_t eepromSize) {
    EEPROM.begin(eepromSize);
    uint8_t magic = 0;
    EEPROM.get(EEPROM_MAGIC_ADDR, magic);
    if (magic != EEPROM_MAGIC) {
        // Write default config on first boot or after wipe
        ton = 0.1f;
        toff = 10.0f;
        strncpy(name, "FogTimer", sizeof(name)-1);
        EEPROM.put(0, ton);
        EEPROM.put(sizeof(float), toff);
            EEPROM.put(sizeof(float)*2, name);
        magic = EEPROM_MAGIC;
        EEPROM.put(EEPROM_MAGIC_ADDR, magic);
        EEPROM.commit();
    }
    load();
}

void DeviceConfig::load() {
    EEPROM.get(0, ton);
    EEPROM.get(sizeof(float), toff);
        EEPROM.get(sizeof(float)*2, name);
    if (ton < 0.1f || ton > 3600.0f) ton = 0.1f;
    if (toff < 0.1f || toff > 3600.0f) toff = 10.0f;
    if (name[0] == '\0') strncpy(name, "FogTimer", sizeof(name)-1);
}

void DeviceConfig::saveTimer(float tOn, float tOff) {
    ton = tOn;
    toff = tOff;
    EEPROM.put(0, ton);
    EEPROM.put(sizeof(float), toff);
    EEPROM.commit();
}

void DeviceConfig::saveName(const char* newName) {
    strncpy(name, newName, sizeof(name)-1);
    name[sizeof(name)-1] = '\0';
    EEPROM.put(sizeof(float)*2, name);
    EEPROM.commit();
}

void DeviceConfig::factoryReset() {
    // Erase EEPROM content to zeros including magic
    for (int i = 0; i < 128; ++i) EEPROM.write(i, 0);
    EEPROM.commit();
    // Re-run begin to write defaults
    begin(128);
}

bool DeviceConfig::isUninitialized() const {
    uint8_t magic = 0; EEPROM.get(EEPROM_MAGIC_ADDR, magic); return magic != EEPROM_MAGIC;
}

float DeviceConfig::getTon() const {
    return ton;
}

float DeviceConfig::getToff() const {
    return toff;
}

const char* DeviceConfig::getName() const {
    return name;
}
