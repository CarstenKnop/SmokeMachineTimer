#pragma once

#include <Arduino.h>
#include <vector>
#include "ReliableSerial.h"
#include "DebugProtocol.h"

class CommManager;
class DeviceManager;
class RemoteChannelManager;

class DebugSerialBridge {
public:
    DebugSerialBridge(CommManager& comm, DeviceManager& devices, RemoteChannelManager& channelMgr);

    void begin(uint32_t baud = 115200);
    void loop();

    void handleTimerPacket(const uint8_t* mac, const DebugProtocol::Packet& packet);

    const DebugProtocol::TimerStatsPayload& getLastTimerStats() const { return lastTimerStats; }
    bool isPcConnected() const { return pcConnected; }

private:
    struct PendingRequest {
        uint16_t requestId = 0;
        uint8_t mac[6] = {0};
        DebugProtocol::Command command = DebugProtocol::Command::Ping;
        uint32_t createdMs = 0;
    };

    CommManager& commManager;
    DeviceManager& deviceManager;
    RemoteChannelManager& channelManager;
    ReliableSerial::Link serialLink;
    std::vector<PendingRequest> pending;
    DebugProtocol::TimerStatsPayload lastTimerStats{};
    uint16_t nextRequestId = 1;
    bool pcConnected = false;
    uint32_t lastTelemetryMs = 0;

    ReliableProtocol::HandlerResult handleSerialFrame(const uint8_t* mac, const uint8_t* payload, size_t len);
    void handlePcPacket(DebugProtocol::Packet& packet);
    void respondToPc(DebugProtocol::Packet& packet, DebugProtocol::Status status);
    void respondError(DebugProtocol::Packet& packet, DebugProtocol::Status status);
    void sendTelemetry();
    void checkPendingTimeouts();
    PendingRequest* findPending(uint16_t requestId);
    PendingRequest& trackPending(uint16_t requestId, const uint8_t* mac, DebugProtocol::Command cmd);
    void completePending(uint16_t requestId);
    uint16_t allocateRequestId();
    void populateRemoteSnapshot(DebugProtocol::TimerSnapshot& snapshot) const;
};
