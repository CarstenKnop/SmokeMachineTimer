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
    uint8_t wifiEnabled = 1; // new: 1=enabled, 0=disabled
    char staSsid[32] = {0};
    char staPass[32] = {0};
    uint8_t apAlwaysOn = 0; // 1 = keep AP running continuously when wifiEnabled
  };

  static constexpr int EEPROM_ADDR = 0;
  static constexpr int EEPROM_ADDR_SAVER = EEPROM_ADDR + sizeof(uint32_t)*2; // after off/on

  bool begin(size_t eepromSize = 32);
  void load();
  void saveTimersIfChanged(uint32_t off, uint32_t on, bool changed);
  void saveScreensaverIfChanged(uint16_t saver);
  void saveWiFiEnabled(uint8_t en);
  void saveStaCreds(const char* ssid, const char* pass);
  void resetWiFi();
  void forgetSta(); // new: only clear station creds
  void saveApAlwaysOn(uint8_t v);

  Values& get() { return vals; }
  const Values& get() const { return vals; }

private:
  Values vals;
  uint16_t lastSavedSaverDelay = 0xFFFF;
  uint8_t lastSavedWifiEnabled = 0xFF;
};
