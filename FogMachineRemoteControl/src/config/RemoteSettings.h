// RemoteSettings.h - the remote's persistent settings (NVS, CRC protected, written only when changed).
#pragma once

#include <Arduino.h>
#include "FogProto.h"
#include "FogStore.h"

struct RemoteSettingsData {
    int8_t txPowerQdbm = 80;                   // 0.25 dBm units (80 = 20 dBm, the EU limit)
    uint8_t oledBrightness = 255;              // contrast 5..255
    uint16_t autoOffSeconds = 60;              // 0 = never blank / sleep
    int8_t rssiLowDbm = -100;                  // maps to 0 signal bars
    int8_t rssiHighDbm = -80;                  // maps to 6 signal bars
    uint16_t batCalMv[3] = {3300, 3750, 4150}; // battery millivolts that count as 0 %, 50 %, 100 %
    uint8_t net = Fog::kDefaultNet;
    uint8_t channel = Fog::kDefaultChannel;    // system home channel
};

class RemoteSettings {
public:
    void begin();
    const RemoteSettingsData& get() const { return d_; }
    RemoteSettingsData& mut() { return d_; }
    void clamp();
    void save();                               // normalises, then stores if something changed
    void resetToDefaults();

private:
    RemoteSettingsData d_;
    Fog::BlobStore<RemoteSettingsData> store_{"fogremote", "settings", 1};
};
