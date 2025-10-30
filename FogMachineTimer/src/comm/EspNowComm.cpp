// EspNowComm.cpp
// Handles ESP-NOW communication and protocol command processing.
#include "EspNowComm.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <EEPROM.h>
#include <algorithm>
#include <cstring>
#include <cstdint>


namespace {
constexpr uint16_t TIMER_EEPROM_SIZE = 256;
constexpr uint32_t CHANNEL_APPLY_GRACE_MS = 150;
const char* cmdToString(ProtocolCmd cmd) {
    switch (cmd) {
        case ProtocolCmd::PAIR: return "PAIR";
        case ProtocolCmd::STATUS: return "STATUS";
        case ProtocolCmd::SET_TIMER: return "SET_TIMER";
        case ProtocolCmd::OVERRIDE_OUTPUT: return "OVERRIDE_OUTPUT";
        case ProtocolCmd::RESET_STATE: return "RESET_STATE";
        case ProtocolCmd::SET_NAME: return "SET_NAME";
        case ProtocolCmd::GET_RSSI: return "GET_RSSI";
        case ProtocolCmd::CALIBRATE_BATTERY: return "CALIBRATE_BATTERY";
        case ProtocolCmd::TOGGLE_STATE: return "TOGGLE_STATE";
        case ProtocolCmd::FACTORY_RESET: return "FACTORY_RESET";
        case ProtocolCmd::SET_CHANNEL: return "SET_CHANNEL";
        default: return "UNKNOWN";
    }
}

const char* statusToString(ProtocolStatus status) {
    switch (status) {
        case ProtocolStatus::OK: return "OK";
        case ProtocolStatus::INVALID_PARAM: return "INVALID_PARAM";
        case ProtocolStatus::UNSUPPORTED: return "UNSUPPORTED";
        case ProtocolStatus::BUSY: return "BUSY";
        case ProtocolStatus::UNKNOWN_CMD: return "UNKNOWN_CMD";
        default: return "UNSPECIFIED";
    }
}

void* cmdContext(ProtocolCmd cmd) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(cmd));
}

ProtocolCmd contextToCmd(void* ctx) {
    if (!ctx) return ProtocolCmd::STATUS;
    return static_cast<ProtocolCmd>(reinterpret_cast<uintptr_t>(ctx));
}
}


EspNowComm* EspNowComm::instance = nullptr;
volatile int8_t EspNowComm::lastRxRssi = 0;
uint8_t EspNowComm::lastSenderMac[6] = {0};


EspNowComm::EspNowComm(TimerController& timerRef, DeviceConfig& configRef, TimerChannelSettings& channelRef)
    : timer(timerRef), config(configRef), channelSettings(channelRef) {}

void EspNowComm::begin() {
    instance = this;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
    }
    channelSettings.apply();
    esp_now_register_recv_cb(EspNowComm::onDataRecv);
    reliableLink.begin();
    reliableLink.setReceiveHandler([this](const uint8_t* mac, const uint8_t* payload, size_t len) {
        return handleFrame(mac, payload, len);
    });
    reliableLink.setAckCallback([this](const uint8_t* mac, ReliableProtocol::AckType type, uint8_t status, void* ctx, const char* tag) {
        ProtocolCmd cmd = contextToCmd(ctx);
        const char* label = tag ? tag : cmdToString(cmd);
        const char* transportStatus = ReliableProtocol::statusToString(status);
        const char* protocolStatus = statusToString(static_cast<ProtocolStatus>(status));
        const char* statusText = transportStatus ? transportStatus : protocolStatus;
        if (!statusText) statusText = "-";
        switch (type) {
            case ReliableProtocol::AckType::Ack:
                Serial.printf("[SLAVE] ACK %s (%s) status=%u (%s) from %02X:%02X:%02X:%02X:%02X:%02X\n",
                              label,
                              cmdToString(cmd),
                              status,
                              statusText,
                              mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                break;
            case ReliableProtocol::AckType::Nak:
                Serial.printf("[SLAVE] NAK %s (%s) status=%u (%s) from %02X:%02X:%02X:%02X:%02X:%02X\n",
                              label,
                              cmdToString(cmd),
                              status,
                              statusText,
                              mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                break;
            case ReliableProtocol::AckType::Timeout:
                Serial.printf("[SLAVE] TIMEOUT %s (%s) status=%u (%s) from %02X:%02X:%02X:%02X:%02X:%02X\n",
                              label,
                              cmdToString(cmd),
                              status,
                              statusText,
                              mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                break;
        }
    });
    reliableLink.setEnsurePeerCallback([this](const uint8_t* mac) {
        ensurePeer(mac);
    });
    // Enable promiscuous mode to read RSSI of incoming frames
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&EspNowComm::wifiSniffer);
}

void EspNowComm::loop() {
    reliableLink.loop();
    // Push status to the last known sender when the output state changes
    pushStatusIfStateChanged();
    processPendingChannelChange();
}

int8_t EspNowComm::getRssi() const {
    return WiFi.RSSI();
}

void EspNowComm::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!instance) return;
    instance->reliableLink.onReceive(mac, data, len);
}

void EspNowComm::sendStatus(const uint8_t* mac, bool requireAck) {
    ProtocolMsg reply = {};
    reply.cmd = (uint8_t)ProtocolCmd::STATUS;
    reply.ton = config.getTon();
    reply.toff = config.getToff();
    reply.elapsed = timer.getCurrentStateSeconds();
    strncpy(reply.name, config.getName(), sizeof(reply.name)-1);
    reply.outputOverride = timer.isOutputOn();
    reply.resetState = false;
    // Prefer captured RSSI from sniffer for the last sender if available
    reply.rssiAtTimer = lastRxRssi ? lastRxRssi : getRssi();
    reply.channel = channelSettings.getChannel();
    ReliableProtocol::SendConfig cfg;
    cfg.requireAck = requireAck;
    cfg.retryIntervalMs = 200;
    cfg.maxAttempts = requireAck ? 0 : 1;
    cfg.tag = "STATUS";
    cfg.userContext = cmdContext(ProtocolCmd::STATUS);
    reliableLink.sendStruct(mac, reply, cfg);
}

ReliableProtocol::HandlerResult EspNowComm::handleFrame(const uint8_t* mac, const uint8_t* payload, size_t len) {
    ReliableProtocol::HandlerResult result;
    if (!mac) return result;

    if (len == sizeof(DebugProtocol::Packet) && payload[0] == DebugProtocol::PACKET_MAGIC) {
        DebugProtocol::Packet packet;
        memcpy(&packet, payload, sizeof(packet));
        if (!DebugProtocol::isValid(packet)) {
            Serial.println("[SLAVE] Invalid debug packet");
            result.ack = false;
            result.status = static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength);
            return result;
        }
        return handleDebugPacket(mac, packet);
    }

    if (len != sizeof(ProtocolMsg)) {
        Serial.printf("[SLAVE] Dropping payload len=%u (expected %u)\n",
                      static_cast<unsigned>(len), static_cast<unsigned>(sizeof(ProtocolMsg)));
        result.ack = false;
        result.status = static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength);
        return result;
    }

    ProtocolMsg msg = {};
    memcpy(&msg, payload, sizeof(msg));
    ProtocolCmd cmd = static_cast<ProtocolCmd>(msg.cmd);
    Serial.printf("[SLAVE] RX %s from %02X:%02X:%02X:%02X:%02X:%02X len=%u\n",
                  cmdToString(cmd), mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], static_cast<unsigned>(len));
    memcpy(lastSenderMac, mac, 6);
    return processCommand(msg, mac);
}

// Static sniffer callback to capture RSSI
void EspNowComm::wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (!pkt) return;
    // Source MAC is at payload+10 for 802.11 header (addr2)
    if (pkt->rx_ctrl.sig_len < 16) return;
    const uint8_t* src = pkt->payload + 10;
    if (memcmp(src, lastSenderMac, 6) == 0) {
        lastRxRssi = pkt->rx_ctrl.rssi;
    }
}

void EspNowComm::ensurePeer(const uint8_t* mac) {
    if (!mac) return;
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t info = {};
        memcpy(info.peer_addr, mac, 6);
        info.channel = 0;
        info.encrypt = false;
        esp_err_t err = esp_now_add_peer(&info);
        Serial.printf("[SLAVE] Added peer %02X:%02X:%02X:%02X:%02X:%02X (%d)\n",
                      mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], static_cast<int>(err));
    }
}

ReliableProtocol::HandlerResult EspNowComm::processCommand(const ProtocolMsg& msg, const uint8_t* mac) {
    ReliableProtocol::HandlerResult result;
    ProtocolCmd cmd = static_cast<ProtocolCmd>(msg.cmd);
    switch (cmd) {
        case ProtocolCmd::PAIR:
            Serial.println("[SLAVE] PAIR -> sending STATUS");
            sendStatus(mac, true);
            break;
        case ProtocolCmd::SET_TIMER:
            config.saveTimer(msg.ton, msg.toff);
            timer.setTimes(msg.ton, msg.toff);
            sendStatus(mac, true);
            break;
        case ProtocolCmd::OVERRIDE_OUTPUT:
            timer.overrideOutput(msg.outputOverride);
            sendStatus(mac, true);
            break;
        case ProtocolCmd::RESET_STATE:
            timer.resetState();
            sendStatus(mac, true);
            break;
        case ProtocolCmd::TOGGLE_STATE:
            timer.toggleAndReset();
            sendStatus(mac, true);
            break;
        case ProtocolCmd::SET_NAME:
            config.saveName(msg.name);
            sendStatus(mac, true);
            break;
        case ProtocolCmd::SET_CHANNEL: {
            if (!channelSettings.isChannelSupported(msg.channel)) {
                result.ack = false;
                result.status = static_cast<uint8_t>(ProtocolStatus::INVALID_PARAM);
                break;
            }
            bool persist = (msg.reserved[0] & ProtocolFlags::ChannelPersist) != 0;
            bool pendingSame = pendingChannelChange_ && pendingChannelValue_ == msg.channel && pendingChannelPersist_ == persist;
            bool storedUpdated = persist ? channelSettings.storeChannel(msg.channel) : false;

            if (persist) {
                if (storedUpdated || pendingSame) {
                    scheduleChannelApply(msg.channel, mac, true, true);
                } else {
                    channelSettings.apply();
                    sendStatus(mac, true);
                }
            } else {
                if (pendingSame) {
                    scheduleChannelApply(msg.channel, mac, true, false);
                } else if (channelSettings.getChannel() == msg.channel && !pendingChannelChange_) {
                    sendStatus(mac, true);
                } else {
                    scheduleChannelApply(msg.channel, mac, true, false);
                }
            }

            result.ack = true;
            result.status = static_cast<uint8_t>(ProtocolStatus::OK);
            break;
        }
        case ProtocolCmd::FACTORY_RESET:
            Serial.println("[SLAVE] FACTORY_RESET -> wiping EEPROM and restoring defaults");
            config.factoryReset();
            timer.setTimes(config.getTon(), config.getToff());
            channelSettings.resetToDefault();
            sendStatus(mac, true);
            break;
        case ProtocolCmd::GET_RSSI:
            sendStatus(mac, true);
            break;
        default:
            result.ack = false;
            result.status = static_cast<uint8_t>(ProtocolStatus::UNKNOWN_CMD);
            break;
    }
    return result;
}

ReliableProtocol::HandlerResult EspNowComm::handleDebugPacket(const uint8_t* mac, const DebugProtocol::Packet& packet) {
    ReliableProtocol::HandlerResult result;
    DebugProtocol::Packet response = packet;
    response.flags |= static_cast<uint8_t>(DebugProtocol::PacketFlags::Response);
    response.status = DebugProtocol::Status::Ok;

    switch (packet.command) {
        case DebugProtocol::Command::Ping:
            DebugProtocol::clearData(response);
            break;
        case DebugProtocol::Command::GetTimerStats: {
            DebugProtocol::TimerStatsPayload payload = {};
            payload.link.transport = reliableLink.getStats();
            payload.link.rssiLocal = getRssi();
            payload.link.rssiPeer = lastRxRssi;
            payload.link.channel = channelSettings.getChannel();
            payload.timer.tonSeconds = config.getTon();
            payload.timer.toffSeconds = config.getToff();
            payload.timer.elapsedSeconds = timer.getCurrentStateSeconds();
            payload.timer.outputOn = timer.isOutputOn() ? 1 : 0;
            payload.timer.overrideActive = timer.isOverrideActive() ? 1 : 0;
            payload.timer.channel = channelSettings.getChannel();
            DebugProtocol::setData(response, &payload, sizeof(payload));
            break;
        }
        case DebugProtocol::Command::GetRssi: {
            struct RssiReport {
                int8_t timerLocal;
                int8_t lastRemote;
                int8_t reserved0;
                int8_t reserved1;
            } report{};
            report.timerLocal = getRssi();
            report.lastRemote = lastRxRssi;
            DebugProtocol::setData(response, &report, sizeof(report));
            break;
        }
        case DebugProtocol::Command::SetChannel:
        case DebugProtocol::Command::ForceChannel: {
            if (packet.dataLength < 1) {
                response.status = DebugProtocol::Status::InvalidArgument;
                DebugProtocol::clearData(response);
                break;
            }
            uint8_t channel = packet.data[0];
            if (!channelSettings.isChannelSupported(channel)) {
                response.status = DebugProtocol::Status::InvalidArgument;
                DebugProtocol::clearData(response);
            } else {
                bool persist = (packet.command == DebugProtocol::Command::SetChannel);
                bool pendingSame = pendingChannelChange_ && pendingChannelValue_ == channel && pendingChannelPersist_ == persist;
                if (persist) {
                    bool storedUpdated = channelSettings.storeChannel(channel);
                    if (storedUpdated || pendingSame) {
                        scheduleChannelApply(channel, mac, false, true);
                    } else {
                        channelSettings.apply();
                    }
                } else {
                    if (pendingSame) {
                        scheduleChannelApply(channel, mac, false, false);
                    } else {
                        scheduleChannelApply(channel, mac, false, false);
                    }
                }
                DebugProtocol::clearData(response);
            }
            break;
        }
        case DebugProtocol::Command::ReadConfig: {
            if (packet.dataLength < 5) {
                response.status = DebugProtocol::Status::InvalidArgument;
                DebugProtocol::clearData(response);
                break;
            }
            uint16_t address = packet.data[1] | (static_cast<uint16_t>(packet.data[2]) << 8);
            uint16_t length = packet.data[3] | (static_cast<uint16_t>(packet.data[4]) << 8);
            if (address >= TIMER_EEPROM_SIZE) {
                response.status = DebugProtocol::Status::InvalidArgument;
                DebugProtocol::clearData(response);
                break;
            }
            uint16_t capped = std::min<uint16_t>(length, DebugProtocol::MAX_DATA_BYTES);
            if (address + capped > TIMER_EEPROM_SIZE) {
                capped = TIMER_EEPROM_SIZE - address;
            }
            uint8_t buffer[DebugProtocol::MAX_DATA_BYTES] = {0};
            for (uint16_t i = 0; i < capped; ++i) {
                buffer[i] = EEPROM.read(address + i);
            }
            DebugProtocol::setData(response, buffer, capped);
            break;
        }
        case DebugProtocol::Command::WriteConfig: {
            if (packet.dataLength < 5) {
                response.status = DebugProtocol::Status::InvalidArgument;
                DebugProtocol::clearData(response);
                break;
            }
            uint16_t address = packet.data[1] | (static_cast<uint16_t>(packet.data[2]) << 8);
            uint16_t length = packet.data[3] | (static_cast<uint16_t>(packet.data[4]) << 8);
            if (address >= TIMER_EEPROM_SIZE || length + 5 > packet.dataLength) {
                response.status = DebugProtocol::Status::InvalidArgument;
                DebugProtocol::clearData(response);
                break;
            }
            uint16_t capped = std::min<uint16_t>(length, static_cast<uint16_t>(packet.dataLength - 5));
            if (address + capped > TIMER_EEPROM_SIZE) {
                capped = TIMER_EEPROM_SIZE - address;
            }
            const uint8_t* payload = packet.data + 5;
            for (uint16_t i = 0; i < capped; ++i) {
                EEPROM.write(address + i, payload[i]);
            }
            EEPROM.commit();
            DebugProtocol::clearData(response);
            break;
        }
        case DebugProtocol::Command::GetDeviceInfo: {
            DebugProtocol::DeviceInfo info = {};
            info.firmwareVersion = 0x00010002;
            info.buildTimestamp = 20251029;
            info.deviceKind = 1; // timer
            DebugProtocol::setData(response, &info, sizeof(info));
            break;
        }
        default:
            response.status = DebugProtocol::Status::Unsupported;
            DebugProtocol::clearData(response);
            break;
    }

    ReliableProtocol::SendConfig cfg;
    cfg.requireAck = true;
    cfg.retryIntervalMs = 200;
    cfg.maxAttempts = 5;
    cfg.tag = "DEBUG-RSP";
    reliableLink.sendStruct(mac, response, cfg);
    return result;
}

void EspNowComm::pushStatusIfStateChanged() {
    if (!instance) return;
    if (instance->timer.consumeStateChanged()) {
        // We don't have the remote MAC here; for simplicity, broadcast the status so the active remote can catch it.
        uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        instance->ensurePeer(broadcast);
        instance->sendStatus(broadcast, false);
    }
}

void EspNowComm::scheduleChannelApply(uint8_t channel, const uint8_t* mac, bool sendStatus, bool persist) {
    pendingChannelChange_ = true;
    pendingChannelValue_ = channel;
    pendingChannelApplyAtMs_ = millis() + CHANNEL_APPLY_GRACE_MS;
    pendingChannelPersist_ = persist;
    pendingChannelSendStatus_ = pendingChannelSendStatus_ || sendStatus;
    if (mac) {
        memcpy(pendingChannelMac_, mac, sizeof(pendingChannelMac_));
        pendingChannelMacValid_ = true;
    }
}

void EspNowComm::processPendingChannelChange() {
    if (!pendingChannelChange_) {
        return;
    }
    uint32_t now = millis();
    if (static_cast<int32_t>(now - pendingChannelApplyAtMs_) < 0) {
        return;
    }
    if (pendingChannelPersist_) {
        channelSettings.apply();
    } else {
        channelSettings.applyTransient(pendingChannelValue_);
    }
    if (pendingChannelSendStatus_ && pendingChannelMacValid_) {
        sendStatus(pendingChannelMac_, true);
    }
    pendingChannelChange_ = false;
    pendingChannelSendStatus_ = false;
    pendingChannelMacValid_ = false;
    pendingChannelPersist_ = false;
}
