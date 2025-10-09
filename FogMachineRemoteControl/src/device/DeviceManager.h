// DeviceManager.h
// Manages paired slave devices, active selection, names, MACs, and EEPROM persistence.
#pragma once
#include <Arduino.h>
#include <vector>

struct SlaveDevice {
    uint8_t mac[6];
    char name[16];
    int8_t rssiRemote = -127;   // RSSI measured at remote (placeholder)
    int8_t rssiSlave = -127;    // RSSI reported by slave (placeholder)
    float ton = 0.f;
    float toff = 0.f;
    float elapsed = 0.f;        // seconds elapsed in current state (from slave)
    bool outputState = false;
    unsigned long lastStatusMs = 0; // millis() timestamp of last received status
};

class DeviceManager {
public:
    DeviceManager();
    void begin();
    void loadFromEEPROM();
    void saveToEEPROM();
    void addDevice(const SlaveDevice& dev);
    void removeDevice(int index);
    void renameDevice(int index, const char* newName);
    void updateDevice(int index, const SlaveDevice& dev);
    int getDeviceCount() const;
    const SlaveDevice& getDevice(int index) const;
    int findDeviceByMac(const uint8_t mac[6]) const;
    // Active device management
    int getActiveIndex() const { return activeIndex; }
    void setActiveIndex(int idx);
    const SlaveDevice* getActive() const;
    // Update status convenience
    void updateStatus(int index, const SlaveDevice& dev);
    // Wipe all paired devices and reset active selection; persists to EEPROM
    void factoryReset();
private:
    void ensureActiveValid();
    std::vector<SlaveDevice> devices;
    int activeIndex = -1;
};
