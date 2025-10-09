// CommManager.h
// Handles ESP-NOW communication and protocol command processing.
#pragma once
#include <Arduino.h>
#include "Defaults.h"
#include "device/DeviceManager.h"
#include "Pins.h"
#include <vector>

class CommManager {
public:
    CommManager(DeviceManager& deviceMgr);
    void begin();
    void loop();
    void sendCommand(const SlaveDevice& dev, uint8_t cmd, const void* payload, size_t payloadSize);
    void broadcastDiscovery();
    void processIncoming();
    static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);
    // Discovery / pairing
    void startDiscovery(uint32_t durationMs = 8000);
    void stopDiscovery();
    bool isDiscovering() const { return discovering; }
    uint32_t discoveryMsLeft() const { return (discovering && millis() < discoveryEnd) ? (discoveryEnd - millis()) : 0; }
    struct DiscoveredDevice { uint8_t mac[6]; char name[10]; int8_t rssi; float ton; float toff; unsigned long lastSeen; };
    int getDiscoveredCount() const { return (int)discovered.size(); }
    const DiscoveredDevice& getDiscovered(int idx) const { return discovered[idx]; }
    void pairWithIndex(int idx);
    // Status requests
    void requestStatus(const SlaveDevice& dev);
    void requestStatusActive();
    // Control commands for active device
    void resetActive();
    void toggleActive();
    void overrideActive(bool on);
    void setActiveName(const char* newName);
    void setActiveTimer(float tonSec, float toffSec);
    void factoryResetActive();
    // Device management helpers
    const SlaveDevice* getActiveDevice() const { return deviceManager.getActive(); }
    int getPairedCount() const { return deviceManager.getDeviceCount(); }
    const SlaveDevice& getPaired(int i) const { return deviceManager.getDevice(i); }
    void activateDeviceByIndex(int idx) { if (idx>=0 && idx<deviceManager.getDeviceCount()) { deviceManager.setActiveIndex(idx); requestStatus(deviceManager.getDevice(idx)); } }
    void removeDeviceByIndex(int idx) { deviceManager.removeDevice(idx); }
    // Pairing helpers
    int findPairedIndexByMac(const uint8_t mac[6]) const { return deviceManager.findDeviceByMac(mac); }
    void unpairByMac(const uint8_t mac[6]) { int idx = deviceManager.findDeviceByMac(mac); if (idx >= 0) deviceManager.removeDevice(idx); }
    static CommManager* get() { return instance; }
    // RSSI sniffer control (enable only while on RSSI screen)
    void setRssiSnifferEnabled(bool enable);
private:
    DeviceManager& deviceManager;
    static CommManager* instance;
    unsigned long ledBlinkUntil = 0;
    // LED helpers respecting polarity
    inline void commLedOn()  { digitalWrite(COMM_OUT_GPIO, Defaults::COMM_LED_ACTIVE_HIGH ? HIGH : LOW); }
    inline void commLedOff() { digitalWrite(COMM_OUT_GPIO, Defaults::COMM_LED_ACTIVE_HIGH ? LOW  : HIGH); }
    void ensurePeer(const uint8_t mac[6]);
    // Discovery state
    bool discovering = false;
    uint32_t discoveryEnd = 0;
    uint32_t lastDiscoveryPing = 0;
    std::vector<DiscoveredDevice> discovered;
    void addOrUpdateDiscovered(const uint8_t mac[6], const char* name, int8_t rssi, float ton, float toff);
    void finishDiscovery();
    // Status de-dup cache (per MAC tail match)
    struct LastStatusCache { uint8_t mac[6]; float ton; float toff; bool state; unsigned long ts; };
    std::vector<LastStatusCache> lastStatus;
    bool isDuplicateStatus(const uint8_t mac[6], float ton, float toff, bool state, unsigned long now);
    // Promiscuous-mode RSSI capture
    bool snifferEnabled = false;
    static void wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type);
    void noteRssiFromMac(const uint8_t mac[6], int8_t rssi);
};
