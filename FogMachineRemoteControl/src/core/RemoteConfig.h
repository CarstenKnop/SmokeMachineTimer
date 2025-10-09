#pragma once
#include <Arduino.h>
#include <EEPROM.h>

class RemoteConfig {
public:
    struct Values {
        int8_t txPowerQdbm = 84;   // 21.0 dBm (units of 0.25 dBm)
        uint8_t oledBrightness = 255; // 0..255 contrast
    };

    bool begin(size_t eepromSize = 512) {
        EEPROM.begin(eepromSize);
        load();
        return true;
    }

    void load() {
        uint8_t magic=0; EEPROM.get(ADDR_MAGIC, magic);
        if (magic != MAGIC) {
            // write defaults
            vals = Values{};
            save();
            return;
        }
        uint8_t version=0; EEPROM.get(ADDR_VERSION, version);
        (void)version; // reserved for future migrations
        EEPROM.get(ADDR_VALUES, vals);
        // basic sanity
        if (vals.txPowerQdbm < -4) vals.txPowerQdbm = -4;
        if (vals.txPowerQdbm > 84) vals.txPowerQdbm = 84;
    }

    void save() {
        uint8_t magic = MAGIC; EEPROM.put(ADDR_MAGIC, magic);
        uint8_t version = VERSION; EEPROM.put(ADDR_VERSION, version);
        EEPROM.put(ADDR_VALUES, vals);
        EEPROM.commit();
    }

    int8_t getTxPowerQdbm() const { return vals.txPowerQdbm; }
    void setTxPowerQdbm(int8_t qdbm) { vals.txPowerQdbm = qdbm; }
    uint8_t getOledBrightness() const { return vals.oledBrightness; }
    void setOledBrightness(uint8_t v) { vals.oledBrightness = v; }

private:
    static constexpr uint8_t MAGIC = 0xA5;
    static constexpr uint8_t VERSION = 1;
    // Place near the end of 512B EEPROM to avoid colliding with DeviceManager records
    static constexpr int ADDR_BASE   = 400;
    static constexpr int ADDR_MAGIC  = ADDR_BASE + 0;
    static constexpr int ADDR_VERSION= ADDR_BASE + 1;
    static constexpr int ADDR_VALUES = ADDR_BASE + 2;
    Values vals;
};
