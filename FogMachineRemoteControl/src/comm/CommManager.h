// CommManager.h
// Handles ESP-NOW communication and protocol command processing.
#pragma once
#include <Arduino.h>
#include "device/DeviceManager.h"
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
    struct DiscoveredDevice { uint8_t mac[6]; char name[16]; int8_t rssi; float ton; float toff; unsigned long lastSeen; };
    int getDiscoveredCount() const { return (int)discovered.size(); }
    const DiscoveredDevice& getDiscovered(int idx) const { return discovered[idx]; }
    void pairWithIndex(int idx);
    // Status requests
    void requestStatus(const SlaveDevice& dev);
    void requestStatusActive();
    static CommManager* get() { return instance; }
private:
    DeviceManager& deviceManager;
    static CommManager* instance;
    unsigned long ledBlinkUntil = 0;
    // Discovery state
    bool discovering = false;
    uint32_t discoveryEnd = 0;
    uint32_t lastDiscoveryPing = 0;
    std::vector<DiscoveredDevice> discovered;
    void addOrUpdateDiscovered(const uint8_t mac[6], const char* name, int8_t rssi, float ton, float toff);
    void finishDiscovery();
};
