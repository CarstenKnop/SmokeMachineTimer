#pragma once
#include <Arduino.h>
#include <EEPROM.h>
#include "Defaults.h"

// Handles persistence of runtime configurable values.
// Stored layout:
// [0..3]  offTime (uint32_t, tenths)
// [4..7]  onTime  (uint32_t, tenths)
// [8..9]  screensaverDelaySec (uint16_t)

class Config {
public:
  struct Values {
    uint32_t offTime = 100; // 10.0s default
    uint32_t onTime  = 100; // 10.0s default
    uint16_t screensaverDelaySec = 0; // OFF
  };

  static constexpr int EEPROM_ADDR = 0;
  static constexpr int EEPROM_ADDR_SAVER = EEPROM_ADDR + sizeof(uint32_t)*2; // after off/on

  bool begin(size_t eepromSize = 32) {
    EEPROM.begin(eepromSize);
    load();
    return true;
  }

  void load() {
    EEPROM.get(EEPROM_ADDR, vals.offTime);
    EEPROM.get(EEPROM_ADDR + sizeof(uint32_t), vals.onTime);
    EEPROM.get(EEPROM_ADDR_SAVER, vals.screensaverDelaySec);
    if (vals.offTime < Defaults::TIMER_MIN || vals.offTime > Defaults::TIMER_MAX) vals.offTime = 100;
    if (vals.onTime  < Defaults::TIMER_MIN || vals.onTime  > Defaults::TIMER_MAX) vals.onTime  = 100;
    if (vals.screensaverDelaySec > 999) vals.screensaverDelaySec = 0;
    lastSavedSaverDelay = vals.screensaverDelaySec;
  }

  void saveTimersIfChanged(uint32_t off, uint32_t on, bool changed) {
    if (!changed) return;
    vals.offTime = off;
    vals.onTime = on;
    EEPROM.put(EEPROM_ADDR, vals.offTime);
    EEPROM.put(EEPROM_ADDR + sizeof(uint32_t), vals.onTime);
    EEPROM.commit();
  }

  void saveScreensaverIfChanged(uint16_t saver) {
    if (saver == lastSavedSaverDelay) return;
    vals.screensaverDelaySec = saver;
    EEPROM.put(EEPROM_ADDR_SAVER, vals.screensaverDelaySec);
    EEPROM.commit();
    lastSavedSaverDelay = saver;
  }

  Values& get() { return vals; }
  const Values& get() const { return vals; }

private:
  Values vals;
  uint16_t lastSavedSaverDelay = 0xFFFF;
};
