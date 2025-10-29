#include "debug/DebugSerialBridge.h"

#include <WiFi.h>
#include <EEPROM.h>
#include <algorithm>
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
            DebugProtocol::LinkHealth health[2] = {};
            health[0].transport = commManager.getTransportStats();
            health[0].rssiLocal = WiFi.RSSI();
            const SlaveDevice* active = commManager.getActiveDevice();
            health[0].rssiPeer = active ? active->rssiSlave : 0;

            health[1].transport = serialLink.getStats();
            health[1].rssiLocal = WiFi.RSSI();
            health[1].rssiPeer = 0;

            DebugProtocol::setData(packet, health, sizeof(health));
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
            bool informTimer = packet.data[1] != 0 && packet.command == DebugProtocol::Command::SetChannel;
            if (!channelManager.isChannelSupported(newChannel)) {
                respondError(packet, DebugProtocol::Status::InvalidArgument);
                return;
            }
            channelManager.storeChannel(newChannel);
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
    if (packet.command == DebugProtocol::Command::GetTimerStats && packet.dataLength >= sizeof(DebugProtocol::LinkHealth)) {
        memcpy(&lastTimerHealth, packet.data, sizeof(DebugProtocol::LinkHealth));
    }
    if (!pcConnected) {
        return;
    }
    DebugProtocol::Packet forward = packet;
    forward.flags |= static_cast<uint8_t>(DebugProtocol::PacketFlags::Response);
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

    DebugProtocol::LinkHealth health[2] = {};
    health[0].transport = commManager.getTransportStats();
    health[0].rssiLocal = WiFi.RSSI();
    const SlaveDevice* active = commManager.getActiveDevice();
    health[0].rssiPeer = active ? active->rssiSlave : 0;

    health[1] = lastTimerHealth;

    DebugProtocol::setData(packet, health, sizeof(health));
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
