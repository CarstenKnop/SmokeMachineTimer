#include "debug/DebugSerialBridge.h"

#include <WiFi.h>
#include <EEPROM.h>
#include <algorithm>
#include <cstddef>
#include <cctype>
#include <cstring>
#include "comm/CommManager.h"
#include "device/DeviceManager.h"
#include "channel/RemoteChannelManager.h"
#include "Defaults.h"
#include "protocol/Protocol.h"

namespace {
constexpr uint16_t EEPROM_SIZE_BYTES = 512;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 3000;
constexpr uint32_t REQUEST_TIMEOUT_MS = 2000;
constexpr uint32_t REMOTE_FW_VERSION = 0x00010002; // semantic version 0.1.2
constexpr uint32_t REMOTE_BUILD_TIMESTAMP = 20251029; // YYYYMMDD
}

DebugSerialBridge::DebugSerialBridge(CommManager& comm, DeviceManager& devices, RemoteChannelManager& channelMgr)
    : commManager(comm), deviceManager(devices), channelManager(channelMgr) {}

void DebugSerialBridge::begin(uint32_t baud) {
    serialLink.attach(Serial, baud);
    serialLink.setReceiveHandler([this](const uint8_t* mac, const uint8_t* payload, size_t len) {
        return handleSerialFrame(mac, payload, len);
    });
    serialLink.setAckCallback([](const uint8_t*, ReliableProtocol::AckType type, uint8_t status, void*, const char* tag) {
        if (type == ReliableProtocol::AckType::Timeout) {
            Serial.printf("[DEBUG-SERIAL] Timeout sending %s status=%u\n", tag ? tag : "-", status);
        }
    });
}

void DebugSerialBridge::loop() {
    serialLink.loop();
    pcConnected = serialLink.isConnected();
    checkPendingTimeouts();
    sendTelemetry();
}

ReliableProtocol::HandlerResult DebugSerialBridge::handleSerialFrame(const uint8_t*, const uint8_t* payload, size_t len) {
    ReliableProtocol::HandlerResult result;
    if (len != sizeof(DebugProtocol::Packet) || !payload) {
        result.ack = false;
        result.status = static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength);
        return result;
    }
    DebugProtocol::Packet packet;
    memcpy(&packet, payload, sizeof(packet));
    if (!DebugProtocol::isValid(packet)) {
        result.ack = false;
        result.status = static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength);
        return result;
    }
    handlePcPacket(packet);
    return result;
}

void DebugSerialBridge::handlePcPacket(DebugProtocol::Packet& packet) {
    switch (packet.command) {
        case DebugProtocol::Command::Ping: {
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::GetRemoteStats: {
            DebugProtocol::RemoteStatsPayload payload = {};
            payload.remoteLink.transport = commManager.getTransportStats();
            payload.remoteLink.rssiLocal = WiFi.RSSI();
            const SlaveDevice* active = commManager.getActiveDevice();
            payload.remoteLink.rssiPeer = active ? active->rssiSlave : 0;
            payload.remoteLink.channel = channelManager.getActiveChannel();

            const auto& serialStats = serialLink.getStats();
            payload.serialLink.txFrames = serialStats.txFrames;
            payload.serialLink.rxFrames = serialStats.rxFrames;
            payload.serialLink.errors = serialStats.txSendErrors + serialStats.rxCrcErrors + serialStats.rxInvalidLength + serialStats.txTimeout + serialStats.txNak;
            payload.serialLink.lastStatusCode = serialStats.lastStatusCode;

            populateRemoteSnapshot(payload.remote);

            DebugProtocol::setData(packet, &payload, sizeof(payload));
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::GetTimerStats: {
            const SlaveDevice* active = commManager.getActiveDevice();
            if (!active) {
                respondError(packet, DebugProtocol::Status::NotReady);
                return;
            }
            packet.requestId = packet.requestId ? packet.requestId : allocateRequestId();
            PendingRequest& pendingReq = trackPending(packet.requestId, active->mac, packet.command);
            pendingReq.createdMs = millis();

            ReliableProtocol::SendConfig cfg;
            cfg.requireAck = true;
            cfg.retryIntervalMs = Defaults::COMM_RETRY_INTERVAL_MS;
            cfg.maxAttempts = Defaults::COMM_MAX_RETRIES;
            cfg.tag = "DEBUG-TIMER";
            if (!commManager.sendDebugPacket(active->mac, packet, cfg)) {
                completePending(packet.requestId);
                respondError(packet, DebugProtocol::Status::TransportError);
            }
            break;
        }
        case DebugProtocol::Command::SetChannel:
        case DebugProtocol::Command::ForceChannel: {
            if (packet.dataLength < 2) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            uint8_t newChannel = packet.data[0];
            bool informTimer = packet.data[1] != 0;
            bool persist = (packet.command == DebugProtocol::Command::SetChannel);
            if (!channelManager.isChannelSupported(newChannel)) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            if (persist) {
                channelManager.storeChannel(newChannel);
            }
            channelManager.applyChannel(newChannel);
            if (informTimer) {
                const SlaveDevice* active = commManager.getActiveDevice();
                if (active) {
                    ProtocolMsg update = {};
                    update.cmd = static_cast<uint8_t>(ProtocolCmd::SET_CHANNEL);
                    update.channel = newChannel;
                    commManager.sendProtocol(active->mac, update, "DEBUG-SET_CHANNEL", true, nullptr);
                }
            }
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::GetRssi: {
            struct RssiReport {
                int8_t remoteLocal;
                int8_t remoteTimer;
                int8_t timerLocal;
                int8_t reserved;
            } report{};
            report.remoteLocal = WiFi.RSSI();
            const SlaveDevice* active = commManager.getActiveDevice();
            if (active) {
                report.remoteTimer = active->rssiSlave;
                report.timerLocal = active->rssiRemote;
            }
            DebugProtocol::setData(packet, &report, sizeof(report));
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::ReadConfig: {
            if (packet.dataLength < 5) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            uint8_t target = packet.data[0];
            uint16_t address = packet.data[1] | (static_cast<uint16_t>(packet.data[2]) << 8);
            uint16_t length = packet.data[3] | (static_cast<uint16_t>(packet.data[4]) << 8);
            if (target == 0) {
                if (address >= EEPROM_SIZE_BYTES) {
                    respondError(packet, DebugProtocol::Status::InvalidArgument);
                    return;
                }
                uint16_t capped = std::min<uint16_t>(length, DebugProtocol::MAX_DATA_BYTES);
                if (address + capped > EEPROM_SIZE_BYTES) {
                    capped = EEPROM_SIZE_BYTES - address;
                }
                uint8_t buffer[DebugProtocol::MAX_DATA_BYTES] = {0};
                for (uint16_t i = 0; i < capped; ++i) {
                    buffer[i] = EEPROM.read(address + i);
                }
                DebugProtocol::setData(packet, buffer, capped);
                respondToPc(packet, DebugProtocol::Status::Ok);
            } else {
                const SlaveDevice* active = commManager.getActiveDevice();
                if (!active) {
                    respondError(packet, DebugProtocol::Status::NotReady);
                    return;
                }
                packet.requestId = packet.requestId ? packet.requestId : allocateRequestId();
                PendingRequest& pendingReq = trackPending(packet.requestId, active->mac, packet.command);
                pendingReq.createdMs = millis();
                ReliableProtocol::SendConfig cfg;
                cfg.requireAck = true;
                cfg.retryIntervalMs = Defaults::COMM_RETRY_INTERVAL_MS;
                cfg.maxAttempts = Defaults::COMM_MAX_RETRIES;
                cfg.tag = "DEBUG-READCFG";
                if (!commManager.sendDebugPacket(active->mac, packet, cfg)) {
                    completePending(packet.requestId);
                    respondError(packet, DebugProtocol::Status::TransportError);
                }
            }
            break;
        }
        case DebugProtocol::Command::WriteConfig: {
            if (packet.dataLength < 5) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            uint8_t target = packet.data[0];
            uint16_t address = packet.data[1] | (static_cast<uint16_t>(packet.data[2]) << 8);
            uint16_t length = packet.data[3] | (static_cast<uint16_t>(packet.data[4]) << 8);
            if (length + 5 > packet.dataLength) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            const uint8_t* payload = packet.data + 5;
            if (target == 0) {
                if (address >= EEPROM_SIZE_BYTES) {
                    respondError(packet, DebugProtocol::Status::InvalidArgument);
                    return;
                }
                uint16_t capped = std::min<uint16_t>(length, static_cast<uint16_t>(packet.dataLength - 5));
                if (address + capped > EEPROM_SIZE_BYTES) {
                    capped = EEPROM_SIZE_BYTES - address;
                }
                for (uint16_t i = 0; i < capped; ++i) {
                    EEPROM.write(address + i, payload[i]);
                }
                EEPROM.commit();
                respondToPc(packet, DebugProtocol::Status::Ok);
            } else {
                const SlaveDevice* active = commManager.getActiveDevice();
                if (!active) {
                    respondError(packet, DebugProtocol::Status::NotReady);
                    return;
                }
                packet.requestId = packet.requestId ? packet.requestId : allocateRequestId();
                PendingRequest& pendingReq = trackPending(packet.requestId, active->mac, packet.command);
                pendingReq.createdMs = millis();
                ReliableProtocol::SendConfig cfg;
                cfg.requireAck = true;
                cfg.retryIntervalMs = Defaults::COMM_RETRY_INTERVAL_MS;
                cfg.maxAttempts = Defaults::COMM_MAX_RETRIES;
                cfg.tag = "DEBUG-WRITECFG";
                if (!commManager.sendDebugPacket(active->mac, packet, cfg)) {
                    completePending(packet.requestId);
                    respondError(packet, DebugProtocol::Status::TransportError);
                }
            }
            break;
        }
        case DebugProtocol::Command::GetDeviceInfo: {
            DebugProtocol::DeviceInfo info = {};
            info.firmwareVersion = REMOTE_FW_VERSION;
            info.buildTimestamp = REMOTE_BUILD_TIMESTAMP;
            info.deviceKind = 0; // remote
            DebugProtocol::setData(packet, &info, sizeof(info));
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::GetDeviceInventory: {
            uint8_t start = (packet.dataLength >= 1) ? packet.data[0] : 0;
            int total = deviceManager.getDeviceCount();
            if (start >= static_cast<uint8_t>(std::max(total, 0))) {
                start = static_cast<uint8_t>(total);
            }
            DebugProtocol::DeviceInventoryPayload payload = {};
            payload.totalCount = static_cast<uint8_t>(std::min(total, 255));
            payload.batchStart = start;
            int activeIdx = deviceManager.getActiveIndex();
            payload.activeIndex = (activeIdx >= 0) ? static_cast<uint8_t>(activeIdx) : 0xFF;
            uint8_t batchCount = 0;
            for (int idx = start; idx < total && batchCount < DebugProtocol::DeviceInventoryPayload::kMaxEntries; ++idx) {
                const SlaveDevice& dev = deviceManager.getDevice(idx);
                DebugProtocol::DeviceInventoryEntry& entry = payload.entries[batchCount];
                entry.index = static_cast<uint8_t>(idx);
                entry.channel = channelManager.getActiveChannel();
                memcpy(entry.mac, dev.mac, sizeof(entry.mac));
                memset(entry.name, 0, sizeof(entry.name));
                strncpy(entry.name, dev.name, sizeof(entry.name) - 1);
                batchCount++;
            }
            payload.batchCount = batchCount;
            constexpr size_t headerSize = offsetof(DebugProtocol::DeviceInventoryPayload, entries);
            size_t payloadSize = headerSize + static_cast<size_t>(batchCount) * sizeof(DebugProtocol::DeviceInventoryEntry);
            DebugProtocol::setData(packet, &payload, payloadSize);
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::SelectDevice: {
            if (packet.dataLength < 1) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            uint8_t index = packet.data[0];
            if (index >= static_cast<uint8_t>(std::max(deviceManager.getDeviceCount(), 0))) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            commManager.activateDeviceByIndex(index);
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::StartDiscovery: {
            uint32_t durationMs = 0;
            if (packet.dataLength >= 4) {
                durationMs = static_cast<uint32_t>(packet.data[0]) |
                              (static_cast<uint32_t>(packet.data[1]) << 8) |
                              (static_cast<uint32_t>(packet.data[2]) << 16) |
                              (static_cast<uint32_t>(packet.data[3]) << 24);
            }
            commManager.startDiscovery(durationMs);
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::StopDiscovery: {
            commManager.stopDiscovery();
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::GetDiscoveredDevices: {
            uint8_t start = (packet.dataLength >= 1) ? packet.data[0] : 0;
            int discoveredCount = commManager.getDiscoveredCount();
            if (start >= static_cast<uint8_t>(std::max(discoveredCount, 0))) {
                start = static_cast<uint8_t>(discoveredCount);
            }
            DebugProtocol::DiscoveredDevicesPayload payload = {};
            payload.totalCount = static_cast<uint8_t>(std::min(discoveredCount, 255));
            payload.batchStart = start;
            uint8_t batchCount = 0;
            for (int idx = start; idx < discoveredCount && batchCount < DebugProtocol::DiscoveredDevicesPayload::kMaxEntries; ++idx) {
                const auto& disc = commManager.discovered[static_cast<size_t>(idx)];
                DebugProtocol::DiscoveredDeviceEntry& entry = payload.entries[batchCount];
                entry.discoveryIndex = static_cast<uint8_t>(idx);
                entry.channel = disc.channel;
                entry.rssi = disc.rssi;
                memcpy(entry.mac, disc.mac, sizeof(entry.mac));
                memset(entry.timerName, 0, sizeof(entry.timerName));
                strncpy(entry.timerName, disc.name, sizeof(entry.timerName) - 1);
                int pairedIndex = deviceManager.findDeviceByMac(disc.mac);
                entry.pairedIndex = pairedIndex >= 0 ? static_cast<uint8_t>(pairedIndex) : 0xFF;
                if (pairedIndex >= 0) {
                    const SlaveDevice& dev = deviceManager.getDevice(pairedIndex);
                    memset(entry.remoteName, 0, sizeof(entry.remoteName));
                    strncpy(entry.remoteName, dev.name, sizeof(entry.remoteName) - 1);
                } else {
                    memset(entry.remoteName, 0, sizeof(entry.remoteName));
                }
                batchCount++;
            }
            payload.batchCount = batchCount;
            constexpr size_t headerSize = offsetof(DebugProtocol::DiscoveredDevicesPayload, entries);
            size_t payloadSize = headerSize + static_cast<size_t>(batchCount) * sizeof(DebugProtocol::DiscoveredDeviceEntry);
            DebugProtocol::setData(packet, &payload, payloadSize);
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::PairDiscoveredDevice: {
            if (packet.dataLength < 1) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            uint8_t index = packet.data[0];
            if (index >= static_cast<uint8_t>(std::max(commManager.getDiscoveredCount(), 0))) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            commManager.pairWithIndex(index);
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::UnpairDevice: {
            if (packet.dataLength < 1) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            uint8_t index = packet.data[0];
            if (index >= static_cast<uint8_t>(std::max(deviceManager.getDeviceCount(), 0))) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            commManager.removeDeviceByIndex(index);
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::RenameDevice: {
            if (packet.dataLength < 2) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            uint8_t index = packet.data[0];
            if (index >= static_cast<uint8_t>(std::max(deviceManager.getDeviceCount(), 0))) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            char nameBuf[sizeof(SlaveDevice::name)] = {};
            size_t copyLen = std::min<size_t>(packet.dataLength - 1, sizeof(nameBuf) - 1);
            memcpy(nameBuf, packet.data + 1, copyLen);
            nameBuf[copyLen] = '\0';
            // Trim trailing whitespace
            for (int i = static_cast<int>(copyLen) - 1; i >= 0; --i) {
                if (nameBuf[i] == '\0') continue;
                if (std::isspace(static_cast<unsigned char>(nameBuf[i]))) {
                    nameBuf[i] = '\0';
                    continue;
                }
                break;
            }
            if (nameBuf[0] == '\0') {
                strncpy(nameBuf, "Timer", sizeof(nameBuf) - 1);
            }
            commManager.renameDeviceByIndex(index, nameBuf);
            respondToPc(packet, DebugProtocol::Status::Ok);
            break;
        }
        case DebugProtocol::Command::GetLogSnapshot: {
            respondError(packet, DebugProtocol::Status::Unsupported);
            break;
        }
        default:
            respondError(packet, DebugProtocol::Status::Unsupported);
            break;
    }
}

void DebugSerialBridge::respondToPc(DebugProtocol::Packet& packet, DebugProtocol::Status status) {
    packet.status = status;
    packet.flags |= static_cast<uint8_t>(DebugProtocol::PacketFlags::Response);
    ReliableProtocol::SendConfig cfg;
    cfg.requireAck = true;
    cfg.retryIntervalMs = 100;
    cfg.maxAttempts = 10;
    cfg.tag = "DEBUG-PC";
    serialLink.sendStruct(packet, cfg);
}

void DebugSerialBridge::respondError(DebugProtocol::Packet& packet, DebugProtocol::Status status) {
    DebugProtocol::clearData(packet);
    respondToPc(packet, status);
}

void DebugSerialBridge::handleTimerPacket(const uint8_t* mac, const DebugProtocol::Packet& packet) {
    if (packet.command == DebugProtocol::Command::GetTimerStats &&
        packet.dataLength >= sizeof(DebugProtocol::TimerStatsPayload)) {
        memcpy(&lastTimerStats, packet.data, sizeof(DebugProtocol::TimerStatsPayload));
        populateRemoteSnapshot(lastTimerStats.remote);
    }
    if (!pcConnected) {
        return;
    }
    DebugProtocol::Packet forward = packet;
    forward.flags |= static_cast<uint8_t>(DebugProtocol::PacketFlags::Response);
    if (packet.command == DebugProtocol::Command::GetTimerStats &&
        packet.dataLength >= sizeof(DebugProtocol::TimerStatsPayload)) {
        DebugProtocol::setData(forward, &lastTimerStats, sizeof(lastTimerStats));
    }
    ReliableProtocol::SendConfig cfg;
    cfg.requireAck = true;
    cfg.retryIntervalMs = 100;
    cfg.maxAttempts = 10;
    cfg.tag = "DEBUG-PC-FWD";
    serialLink.sendStruct(forward, cfg);
    if (packet.requestId) {
        completePending(packet.requestId);
    }
}

void DebugSerialBridge::sendTelemetry() {
    if (!pcConnected) return;
    const uint32_t now = millis();
    if (now - lastTelemetryMs < TELEMETRY_INTERVAL_MS) return;
    lastTelemetryMs = now;

    DebugProtocol::Packet packet = {};
    packet.magic = DebugProtocol::PACKET_MAGIC;
    packet.command = DebugProtocol::Command::GetRemoteStats;
    packet.flags = static_cast<uint8_t>(DebugProtocol::PacketFlags::Response | DebugProtocol::PacketFlags::Streaming);
    packet.status = DebugProtocol::Status::Ok;

    DebugProtocol::RemoteStatsPayload payload = {};
    payload.remoteLink.transport = commManager.getTransportStats();
    payload.remoteLink.rssiLocal = WiFi.RSSI();
    const SlaveDevice* active = commManager.getActiveDevice();
    payload.remoteLink.rssiPeer = active ? active->rssiSlave : 0;
    payload.remoteLink.channel = channelManager.getActiveChannel();

    const auto& serialStats = serialLink.getStats();
    payload.serialLink.txFrames = serialStats.txFrames;
    payload.serialLink.rxFrames = serialStats.rxFrames;
    payload.serialLink.errors = serialStats.txSendErrors + serialStats.rxCrcErrors + serialStats.rxInvalidLength + serialStats.txTimeout + serialStats.txNak;
    payload.serialLink.lastStatusCode = serialStats.lastStatusCode;

    populateRemoteSnapshot(payload.remote);

    DebugProtocol::setData(packet, &payload, sizeof(payload));
    ReliableProtocol::SendConfig cfg;
    cfg.requireAck = false;
    cfg.tag = "DEBUG-TELEM";
    serialLink.sendStruct(packet, cfg);
}

void DebugSerialBridge::checkPendingTimeouts() {
    const uint32_t now = millis();
    size_t index = 0;
    while (index < pending.size()) {
        PendingRequest& req = pending[index];
        if (now - req.createdMs > REQUEST_TIMEOUT_MS) {
            DebugProtocol::Packet packet = {};
            packet.magic = DebugProtocol::PACKET_MAGIC;
            packet.command = req.command;
            packet.requestId = req.requestId;
            respondError(packet, DebugProtocol::Status::Timeout);
            pending.erase(pending.begin() + index);
        } else {
            ++index;
        }
    }
}

DebugSerialBridge::PendingRequest* DebugSerialBridge::findPending(uint16_t requestId) {
    for (auto& req : pending) {
        if (req.requestId == requestId) {
            return &req;
        }
    }
    return nullptr;
}

DebugSerialBridge::PendingRequest& DebugSerialBridge::trackPending(uint16_t requestId, const uint8_t* mac, DebugProtocol::Command cmd) {
    PendingRequest* existing = findPending(requestId);
    if (existing) {
        return *existing;
    }
    PendingRequest entry;
    entry.requestId = requestId;
    if (mac) {
        memcpy(entry.mac, mac, 6);
    }
    entry.command = cmd;
    entry.createdMs = millis();
    pending.push_back(entry);
    return pending.back();
}

void DebugSerialBridge::completePending(uint16_t requestId) {
    for (size_t i = 0; i < pending.size(); ++i) {
        if (pending[i].requestId == requestId) {
            pending.erase(pending.begin() + i);
            return;
        }
    }
}

uint16_t DebugSerialBridge::allocateRequestId() {
    if (++nextRequestId == 0) {
        nextRequestId = 1;
    }
    return nextRequestId;
}

void DebugSerialBridge::populateRemoteSnapshot(DebugProtocol::TimerSnapshot& snapshot) const {
    snapshot = {};
    snapshot.channel = channelManager.getActiveChannel();
    const SlaveDevice* active = commManager.getActiveDevice();
    if (!active) {
        return;
    }
    snapshot.tonSeconds = active->ton;
    snapshot.toffSeconds = active->toff;
    snapshot.elapsedSeconds = active->elapsed;
    snapshot.outputOn = active->outputState ? 1 : 0;
    snapshot.overrideActive = 0;
}
