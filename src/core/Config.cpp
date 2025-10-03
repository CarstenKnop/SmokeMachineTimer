#include "Config.h"

bool Config::begin(size_t eepromSize) {
    if (eepromSize < 80) eepromSize = 80; // ensure enough space for extended layout
    EEPROM.begin(eepromSize);
    load();
    return true;
}

void Config::load() {
    EEPROM.get(EEPROM_ADDR, vals.offTime);
    EEPROM.get(EEPROM_ADDR + sizeof(uint32_t), vals.onTime);
    EEPROM.get(EEPROM_ADDR_SAVER, vals.screensaverDelaySec);
    int wifiBase = EEPROM_ADDR_SAVER + sizeof(uint16_t);
    // Layout extension: [saver(2)] [wifiEnabled(1)] [ssid(32)] [pass(32)] [apAlwaysOn(1)]
    EEPROM.get(wifiBase, vals.wifiEnabled);
    if (vals.wifiEnabled > 1) vals.wifiEnabled = 1;
    EEPROM.get(wifiBase+1, vals.staSsid);
    EEPROM.get(wifiBase+1+32, vals.staPass);
    EEPROM.get(wifiBase+1+32+32, vals.apAlwaysOn);
    if (vals.apAlwaysOn>1) vals.apAlwaysOn=0;
    if (vals.offTime < Defaults::TIMER_MIN || vals.offTime > Defaults::TIMER_MAX) vals.offTime = 100;
    if (vals.onTime  < Defaults::TIMER_MIN || vals.onTime  > Defaults::TIMER_MAX) vals.onTime  = 100;
    if (vals.screensaverDelaySec > 999) vals.screensaverDelaySec = 0;
    lastSavedSaverDelay = vals.screensaverDelaySec;
    lastSavedWifiEnabled = vals.wifiEnabled;
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

void Config::saveWiFiEnabled(uint8_t en) {
    if (en>1) en=1; if (en==lastSavedWifiEnabled) return;
    vals.wifiEnabled = en; int wifiBase = EEPROM_ADDR_SAVER + sizeof(uint16_t);
    EEPROM.put(wifiBase, vals.wifiEnabled);
    EEPROM.commit();
    lastSavedWifiEnabled = en;
}

void Config::saveStaCreds(const char* ssid, const char* pass) {
    int wifiBase = EEPROM_ADDR_SAVER + sizeof(uint16_t);
    if (ssid) { strncpy(vals.staSsid, ssid, sizeof(vals.staSsid)-1); vals.staSsid[sizeof(vals.staSsid)-1]='\0'; }
    if (pass) { strncpy(vals.staPass, pass, sizeof(vals.staPass)-1); vals.staPass[sizeof(vals.staPass)-1]='\0'; }
    EEPROM.put(wifiBase+1, vals.staSsid);
    EEPROM.put(wifiBase+1+32, vals.staPass);
    EEPROM.commit();
}

void Config::resetWiFi() {
    vals.wifiEnabled = 0; vals.staSsid[0]='\0'; vals.staPass[0]='\0';
    int wifiBase = EEPROM_ADDR_SAVER + sizeof(uint16_t);
    EEPROM.put(wifiBase, vals.wifiEnabled);
    EEPROM.put(wifiBase+1, vals.staSsid);
    EEPROM.put(wifiBase+1+32, vals.staPass);
    EEPROM.commit();
    lastSavedWifiEnabled = vals.wifiEnabled;
}

void Config::forgetSta() {
    // Preserve wifiEnabled flag; only clear credentials
    vals.staSsid[0]='\0'; vals.staPass[0]='\0';
    int wifiBase = EEPROM_ADDR_SAVER + sizeof(uint16_t);
    EEPROM.put(wifiBase+1, vals.staSsid);
    EEPROM.put(wifiBase+1+32, vals.staPass);
    EEPROM.commit();
}

void Config::saveApAlwaysOn(uint8_t v) {
    if (v>1) v=1; if (vals.apAlwaysOn==v) return; vals.apAlwaysOn=v;
    int wifiBase = EEPROM_ADDR_SAVER + sizeof(uint16_t);
    EEPROM.put(wifiBase+1+32+32, vals.apAlwaysOn);
    EEPROM.commit();
}
