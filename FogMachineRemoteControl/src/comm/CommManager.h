// CommManager.h
// Handles ESP-NOW communication and protocol command processing.
#pragma once
#include <Arduino.h>
#include "Defaults.h"
#include "device/DeviceManager.h"
#include "Pins.h"
#include "ReliableEspNow.h"
#include "ReliableProtocol.h"
#include "DebugProtocol.h"
#include "protocol/Protocol.h"
#include <vector>

class RemoteChannelManager;
class DebugSerialBridge;

class CommManager {
public:
    CommManager(DeviceManager& deviceMgr, RemoteChannelManager& channelMgr);
    void begin();
    void loop();
    void sendCommand(const SlaveDevice& dev, uint8_t cmd, const void* payload, size_t payloadSize);
    void broadcastDiscovery();
    void processIncoming();
    static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);
    void attachDebugBridge(DebugSerialBridge* bridge) { debugBridge = bridge; }
    bool sendDebugPacket(const uint8_t* mac, const DebugProtocol::Packet& packet, const ReliableProtocol::SendConfig& cfg = ReliableProtocol::SendConfig{});
    const ReliableProtocol::TransportStats& getTransportStats() const { return reliableLink.getStats(); }
    void resetTransportStats() { reliableLink.resetStats(); }
    // Discovery / pairing
    void startDiscovery(uint32_t durationMs = 8000);
    void stopDiscovery();
    bool isDiscovering() const { return discovering; }
    uint32_t discoveryMsLeft() const { return (discovering && millis() < discoveryEnd) ? (discoveryEnd - millis()) : 0; }
    struct DiscoveredDevice { uint8_t mac[6]; char name[10]; int8_t rssi; float ton; float toff; unsigned long lastSeen; uint8_t channel; };
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
    void onChannelChanged(uint8_t previousChannel);
    // Device management helpers
    const SlaveDevice* getActiveDevice() const { return deviceManager.getActive(); }
    int getPairedCount() const { return deviceManager.getDeviceCount(); }
    const SlaveDevice& getPaired(int i) const { return deviceManager.getDevice(i); }
    void activateDeviceByIndex(int idx) { if (idx>=0 && idx<deviceManager.getDeviceCount()) { deviceManager.setActiveIndex(idx); requestStatus(deviceManager.getDevice(idx)); } }
    void removeDeviceByIndex(int idx) { deviceManager.removeDevice(idx); }
    // Pairing helpers
    int findPairedIndexByMac(const uint8_t mac[6]) const { return deviceManager.findDeviceByMac(mac); }
    void unpairByMac(const uint8_t mac[6]) { int idx = deviceManager.findDeviceByMac(mac); if (idx >= 0) deviceManager.removeDevice(idx); }
    void renameDeviceByIndex(int idx, const char* newName);
    bool programTimerByIndex(int idx, float tonSec, float toffSec);
    bool setOverrideStateByIndex(int idx, bool on);
    static CommManager* get() { return instance; }
    // RSSI sniffer control (enable only while on RSSI screen)
    void setRssiSnifferEnabled(bool enable);
private:
    DeviceManager& deviceManager;
    RemoteChannelManager& channelManager;
    static CommManager* instance;
    unsigned long ledBlinkUntil = 0;
    // LED helpers respecting polarity
    inline void commLedOn()  { digitalWrite(COMM_OUT_GPIO, Defaults::COMM_LED_ACTIVE_HIGH ? HIGH : LOW); }
    inline void commLedOff() { digitalWrite(COMM_OUT_GPIO, Defaults::COMM_LED_ACTIVE_HIGH ? LOW  : HIGH); }
    void ensurePeer(const uint8_t mac[6]);
    ReliableProtocol::HandlerResult handleFrame(const uint8_t* mac, const uint8_t* payload, size_t len);
    ReliableProtocol::HandlerResult handleDebugPacket(const uint8_t* mac, const DebugProtocol::Packet& packet);
    void handleAck(const uint8_t* mac, ReliableProtocol::AckType type, uint8_t status, void* context, const char* tag);
    bool sendProtocol(const uint8_t* mac, ProtocolMsg& msg, const char* tag, bool requireAck = true, void* context = nullptr);
    ReliableEspNow::Link reliableLink;
    DebugSerialBridge* debugBridge = nullptr;
    // Discovery state
    bool discovering = false;
    uint32_t discoveryEnd = 0;
    uint32_t lastDiscoveryPing = 0;
    std::vector<DiscoveredDevice> discovered;
    std::vector<uint8_t> discoveryChannels;
    size_t discoveryChannelIndex = 0;
    unsigned long discoveryChannelUntil = 0;
    void addOrUpdateDiscovered(const uint8_t mac[6], const char* name, int8_t rssi, float ton, float toff, uint8_t channel);
    void finishDiscovery();
    // Status de-dup cache (per MAC tail match)
    struct LastStatusCache { uint8_t mac[6]; float ton; float toff; bool state; unsigned long ts; };
    std::vector<LastStatusCache> lastStatus;
    bool isDuplicateStatus(const uint8_t mac[6], float ton, float toff, bool state, unsigned long now);
    // Promiscuous-mode RSSI capture
    bool snifferEnabled = false;
    static void wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type);
    void noteRssiFromMac(const uint8_t mac[6], int8_t rssi);
    void sendChannelUpdate(const uint8_t mac[6]);
    void switchDiscoveryChannel(uint8_t channel);
    static constexpr unsigned long DISCOVERY_DWELL_MS = 700;
    friend class DebugSerialBridge;
};
