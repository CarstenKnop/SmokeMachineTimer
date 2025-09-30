#include "Config.h"

bool Config::begin(size_t eepromSize) {
    EEPROM.begin(eepromSize);
    load();
    return true;
}

void Config::load() {
    EEPROM.get(EEPROM_ADDR, vals.offTime);
    EEPROM.get(EEPROM_ADDR + sizeof(uint32_t), vals.onTime);
    EEPROM.get(EEPROM_ADDR_SAVER, vals.screensaverDelaySec);
    if (vals.offTime < Defaults::TIMER_MIN || vals.offTime > Defaults::TIMER_MAX) vals.offTime = 100;
    if (vals.onTime  < Defaults::TIMER_MIN || vals.onTime  > Defaults::TIMER_MAX) vals.onTime  = 100;
    if (vals.screensaverDelaySec > 999) vals.screensaverDelaySec = 0;
    lastSavedSaverDelay = vals.screensaverDelaySec;
}

void Config::saveTimersIfChanged(uint32_t off, uint32_t on, bool changed) {
    if (!changed) return;
    vals.offTime = off;
    vals.onTime = on;
    EEPROM.put(EEPROM_ADDR, vals.offTime);
    EEPROM.put(EEPROM_ADDR + sizeof(uint32_t), vals.onTime);
    EEPROM.commit();
}

void Config::saveScreensaverIfChanged(uint16_t saver) {
    if (saver == lastSavedSaverDelay) return;
    vals.screensaverDelaySec = saver;
    EEPROM.put(EEPROM_ADDR_SAVER, vals.screensaverDelaySec);
    EEPROM.commit();
    lastSavedSaverDelay = saver;
}
