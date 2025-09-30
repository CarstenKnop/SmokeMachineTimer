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

  bool begin(size_t eepromSize = 32);
  void load();
  void saveTimersIfChanged(uint32_t off, uint32_t on, bool changed);
  void saveScreensaverIfChanged(uint16_t saver);

  Values& get() { return vals; }
  const Values& get() const { return vals; }

private:
  Values vals;
  uint16_t lastSavedSaverDelay = 0xFFFF;
};
