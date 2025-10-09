// DeviceManager.cpp
// Manages paired slave devices, active selection, and EEPROM persistence.
#include "DeviceManager.h"
#include <EEPROM.h>
#include <string.h>

// EEPROM layout (simple, not wear-levelled):
// [0] count (uint8)
// [1] activeIndex (int8, -1 if none) -> store as uint8_t with 255 meaning -1
// Then per device: mac[6], name[16]
// NOTE: Extend later for additional persisted fields.

static constexpr int EEPROM_ADDR_COUNT = 0;
static constexpr int EEPROM_ADDR_ACTIVE = 1;
static constexpr int EEPROM_ADDR_DEVICES = 2; // start of device records

DeviceManager::DeviceManager() {}

void DeviceManager::begin() {
    loadFromEEPROM();
}

void DeviceManager::loadFromEEPROM() {
    devices.clear();
    uint8_t count = 0; EEPROM.get(EEPROM_ADDR_COUNT, count);
    uint8_t activeRaw = 255; EEPROM.get(EEPROM_ADDR_ACTIVE, activeRaw);
    int base = EEPROM_ADDR_DEVICES;
    for (int i = 0; i < count; ++i) {
        SlaveDevice dev = {};
        EEPROM.get(base, dev.mac); base += 6;
        EEPROM.get(base, dev.name); base += 16;
        devices.push_back(dev);
    }
    activeIndex = (activeRaw == 255) ? -1 : (int)activeRaw;
    ensureActiveValid();
}

void DeviceManager::saveToEEPROM() {
    uint8_t count = (uint8_t)devices.size();
    EEPROM.put(EEPROM_ADDR_COUNT, count);
    uint8_t activeRaw = (activeIndex < 0) ? 255 : (uint8_t)activeIndex;
    EEPROM.put(EEPROM_ADDR_ACTIVE, activeRaw);
    int base = EEPROM_ADDR_DEVICES;
    for (int i = 0; i < count; ++i) {
        EEPROM.put(base, devices[i].mac); base += 6;
        EEPROM.put(base, devices[i].name); base += 16;
    }
    EEPROM.commit();
}

void DeviceManager::addDevice(const SlaveDevice& dev) {
    devices.push_back(dev);
    if (activeIndex < 0) activeIndex = (int)devices.size() - 1; // auto-select first added
    saveToEEPROM();
}

void DeviceManager::removeDevice(int index) {
    if (index >= 0 && index < (int)devices.size()) {
        devices.erase(devices.begin() + index);
        if (activeIndex == index) activeIndex = -1;
        else if (activeIndex > index) activeIndex--; // shift down
        ensureActiveValid();
        saveToEEPROM();
    }
}

void DeviceManager::renameDevice(int index, const char* newName) {
    if (index >= 0 && index < (int)devices.size()) {
        strncpy(devices[index].name, newName, sizeof(devices[index].name)-1);
        devices[index].name[sizeof(devices[index].name)-1] = '\0';
        saveToEEPROM();
    }
}

void DeviceManager::updateDevice(int index, const SlaveDevice& dev) {
    if (index >= 0 && index < (int)devices.size()) {
        devices[index] = dev;
        saveToEEPROM();
    }
}

void DeviceManager::updateStatus(int index, const SlaveDevice& dev) {
    if (index >= 0 && index < (int)devices.size()) {
        devices[index] = dev; // already has lastStatusMs set by caller
        // Do not persist status fields every time to avoid EEPROM wear; skip saveToEEPROM here.
    }
}

int DeviceManager::getDeviceCount() const { return (int)devices.size(); }

const SlaveDevice& DeviceManager::getDevice(int index) const { return devices[index]; }

int DeviceManager::findDeviceByMac(const uint8_t mac[6]) const {
    for (int i = 0; i < (int)devices.size(); ++i) {
        if (memcmp(devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

void DeviceManager::setActiveIndex(int idx) {
    if (idx < 0 || idx >= (int)devices.size()) { activeIndex = -1; }
    else activeIndex = idx;
    saveToEEPROM();
}

const SlaveDevice* DeviceManager::getActive() const {
    if (activeIndex < 0 || activeIndex >= (int)devices.size()) return nullptr;
    return &devices[activeIndex];
}

void DeviceManager::ensureActiveValid() {
    if (devices.empty()) { activeIndex = -1; return; }
    if (activeIndex < 0 || activeIndex >= (int)devices.size()) activeIndex = 0; // default to first
}

void DeviceManager::factoryReset() {
    devices.clear();
    activeIndex = -1;
    // Persist cleared state
    uint8_t zero = 0;
    EEPROM.put(EEPROM_ADDR_COUNT, zero);
    uint8_t inactive = 255; // -1 sentinel
    EEPROM.put(EEPROM_ADDR_ACTIVE, inactive);
    // Optionally clear device slots region (not strictly necessary)
    // Keep it simple to avoid wear; just commit header changes
    EEPROM.commit();
}
