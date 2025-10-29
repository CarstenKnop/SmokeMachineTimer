// CommManager.cpp
// Handles ESP-NOW communication and protocol command processing.
#include "CommManager.h"
#include <WiFi.h>
#include <esp_now.h>
#include <algorithm>
#include <cstdint>
#include "protocol/Protocol.h"
#include "Pins.h"
#include <esp_wifi.h>
#include "Defaults.h"
#include <esp_wifi_types.h>
#include "channel/RemoteChannelManager.h"
#include "debug/DebugSerialBridge.h"

namespace {
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

// Status request helpers (reuse PAIR command as a lightweight status poll)
void CommManager::requestStatus(const SlaveDevice& dev) {
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::PAIR; // interpret as status poll when already paired
    sendProtocol(dev.mac, msg, "STATUS-REQ", true, cmdContext(ProtocolCmd::PAIR));
}

void CommManager::requestStatusActive() {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    requestStatus(*act);
}

void CommManager::resetActive() {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::RESET_STATE;
    sendProtocol(act->mac, msg, "RESET", true, cmdContext(ProtocolCmd::RESET_STATE));
    requestStatus(*act);
}

void CommManager::toggleActive() {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::TOGGLE_STATE;
    sendProtocol(act->mac, msg, "TOGGLE", true, cmdContext(ProtocolCmd::TOGGLE_STATE));
    requestStatus(*act);
}

void CommManager::overrideActive(bool on) {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::OVERRIDE_OUTPUT;
    msg.outputOverride = on;
    sendProtocol(act->mac, msg, "OVERRIDE", true, cmdContext(ProtocolCmd::OVERRIDE_OUTPUT));
}


CommManager* CommManager::instance = nullptr;


CommManager::CommManager(DeviceManager& deviceMgr, RemoteChannelManager& channelMgr)
    : deviceManager(deviceMgr), channelManager(channelMgr) {}

void CommManager::begin() {
    instance = this;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
    }
    channelManager.applyStoredChannel();
    esp_now_register_recv_cb(CommManager::onDataRecv);
    reliableLink.begin();
    reliableLink.setReceiveHandler([this](const uint8_t* mac, const uint8_t* payload, size_t len) {
        return handleFrame(mac, payload, len);
    });
    reliableLink.setAckCallback([this](const uint8_t* mac, ReliableProtocol::AckType type, uint8_t status, void* ctx, const char* tag) {
        handleAck(mac, type, status, ctx, tag);
    });
    reliableLink.setEnsurePeerCallback([this](const uint8_t* mac) {
        ensurePeer(mac);
    });
    reliableLink.setSendHook([this](const uint8_t* /*mac*/) {
        commLedOn();
        ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    });
    // Ensure COMM LED pin is initialized
    pinMode(COMM_OUT_GPIO, OUTPUT);
    // Default off (respect polarity)
    if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, LOW); else digitalWrite(COMM_OUT_GPIO, HIGH);
    // Power-on blink test (3 pulses)
    for (int i=0;i<3;i++) {
        if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
        delay(80);
        if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, LOW); else digitalWrite(COMM_OUT_GPIO, HIGH);
        delay(80);
    }
}

void CommManager::loop() {
    reliableLink.loop();
    // Non-blocking COMM LED blink
    if (ledBlinkUntil && millis() > ledBlinkUntil) {
        if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, LOW); else digitalWrite(COMM_OUT_GPIO, HIGH);
        ledBlinkUntil = 0;
    }
    // Discovery ticking
    if (discovering) {
        uint32_t now = millis();
        if (!discoveryChannels.empty() && now >= discoveryChannelUntil) {
            discoveryChannelIndex = (discoveryChannelIndex + 1) % discoveryChannels.size();
            switchDiscoveryChannel(discoveryChannels[discoveryChannelIndex]);
        }
        if (now - lastDiscoveryPing > 1000) { // ping every second
            broadcastDiscovery();
            lastDiscoveryPing = now;
        }
        // Continuous discovery: when discoveryEnd is max, keep running while in the screen
        if (discoveryEnd != UINT32_MAX && now >= discoveryEnd) {
            finishDiscovery();
        }
    }
}

void CommManager::setRssiSnifferEnabled(bool enable) {
    if (enable == snifferEnabled) return;
    snifferEnabled = enable;
    if (enable) {
        // Enable promiscuous mode to receive packet RSSI
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(&CommManager::wifiSniffer);
    } else {
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        esp_wifi_set_promiscuous(false);
    }
}

// Static sniffer callback
void CommManager::wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!instance) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (!pkt) return;
    const int8_t rssi = pkt->rx_ctrl.rssi;
    const uint8_t* mac = pkt->payload + 10; // addr2 in 802.11 header (source MAC)
    // Basic sanity: payload len must be large enough to contain header
    if (pkt->rx_ctrl.sig_len < 16) return;
    instance->noteRssiFromMac(mac, rssi);
}

void CommManager::noteRssiFromMac(const uint8_t mac[6], int8_t rssi) {
    // If MAC matches any paired device, update its rssiRemote
    int idx = deviceManager.findDeviceByMac(mac);
    if (idx >= 0) {
        SlaveDevice dev = deviceManager.getDevice(idx);
        dev.rssiRemote = rssi;
        deviceManager.updateStatus(idx, dev);
    }
}

ReliableProtocol::HandlerResult CommManager::handleFrame(const uint8_t* mac, const uint8_t* payload, size_t len) {
    ReliableProtocol::HandlerResult result;
    if (!mac) return result;

    if (len == sizeof(DebugProtocol::Packet) && payload[0] == DebugProtocol::PACKET_MAGIC) {
        DebugProtocol::Packet packet;
        memcpy(&packet, payload, sizeof(packet));
        if (!DebugProtocol::isValid(packet)) {
            Serial.println("[COMM] Invalid debug packet");
            result.ack = false;
            result.status = static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength);
            return result;
        }
        return handleDebugPacket(mac, packet);
    }

    if (len != sizeof(ProtocolMsg)) {
        Serial.printf("[COMM] Dropping payload len=%u (expected %u)\n",
                      static_cast<unsigned>(len), static_cast<unsigned>(sizeof(ProtocolMsg)));
        result.ack = false;
        result.status = static_cast<uint8_t>(ReliableProtocol::Status::InvalidLength);
        return result;
    }

    ProtocolMsg msg = {};
    memcpy(&msg, payload, sizeof(msg));
    ProtocolCmd cmd = static_cast<ProtocolCmd>(msg.cmd);
    Serial.printf("[COMM] RX %s from %02X:%02X:%02X:%02X:%02X:%02X len=%u\n",
                  cmdToString(cmd), mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], static_cast<unsigned>(len));

    int8_t rssi = -70; // TODO: capture real RSSI from metadata or sniffer
    uint8_t reportedChannel = msg.channel;
    if (reportedChannel < 1 || reportedChannel > 13) {
        reportedChannel = channelManager.getActiveChannel();
    }

    if (discovering) {
        addOrUpdateDiscovered(mac, msg.name, rssi, msg.ton, msg.toff, reportedChannel);
    }

    if (cmd == ProtocolCmd::STATUS) {
        if (isDuplicateStatus(mac, msg.ton, msg.toff, msg.outputOverride, millis())) {
            return result; // already ack success
        }
    }

    int idx = deviceManager.findDeviceByMac(mac);
    if (idx >= 0) {
        SlaveDevice dev = deviceManager.getDevice(idx);
        dev.ton = msg.ton;
        dev.toff = msg.toff;
        dev.outputState = msg.outputOverride;
        dev.elapsed = msg.elapsed;
        dev.rssiRemote = rssi;
        int8_t rssiTimer = msg.rssiAtTimer;
        if (rssiTimer > 0) rssiTimer = static_cast<int8_t>(-rssiTimer);
        if (rssiTimer < 0 && rssiTimer > -120) {
            dev.rssiSlave = rssiTimer;
        }
        if (msg.name[0]) {
            strncpy(dev.name, msg.name, sizeof(dev.name)-1);
            dev.name[sizeof(dev.name)-1] = '\0';
        }
        dev.lastStatusMs = millis();
        deviceManager.updateStatus(idx, dev);
    }

    return result;
}

void CommManager::sendCommand(const SlaveDevice& dev, uint8_t cmd, const void* payload, size_t payloadSize) {
    ProtocolMsg msg = {};
    msg.cmd = cmd;
    if (payload && payloadSize > 0) {
        size_t copyLen = std::min(payloadSize, sizeof(ProtocolMsg) - sizeof(msg.cmd));
        memcpy(reinterpret_cast<uint8_t*>(&msg) + sizeof(msg.cmd), payload, copyLen);
    }
    sendProtocol(dev.mac, msg, cmdToString(static_cast<ProtocolCmd>(cmd)), true, cmdContext(static_cast<ProtocolCmd>(cmd)));
}

void CommManager::startDiscovery(uint32_t durationMs) {
    discovering = true;
    const uint32_t now = millis();
    discoveryEnd = durationMs ? (now + durationMs) : UINT32_MAX;
    lastDiscoveryPing = 0;
    discovered.clear();
    discoveryChannels.clear();
    discoveryChannelUntil = 0;

    uint8_t preferred = channelManager.getStoredChannel();
    if (preferred >= 1 && preferred <= 13) {
        discoveryChannels.push_back(preferred);
    }
    for (uint8_t ch = 1; ch <= 13; ++ch) {
        if (std::find(discoveryChannels.begin(), discoveryChannels.end(), ch) == discoveryChannels.end()) {
            discoveryChannels.push_back(ch);
        }
    }

    discoveryChannelIndex = 0;
    if (!discoveryChannels.empty()) {
        switchDiscoveryChannel(discoveryChannels[0]);
    }
}

void CommManager::broadcastDiscovery() {
    static constexpr uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ProtocolMsg msg = {};
    msg.cmd = static_cast<uint8_t>(ProtocolCmd::PAIR);
    msg.channel = channelManager.getStoredChannel();
    ReliableProtocol::SendConfig cfg;
    cfg.requireAck = false;
    cfg.tag = "DISCOVERY";
    reliableLink.sendStruct(broadcast, msg, cfg);
}

void CommManager::stopDiscovery() {
    if (!discovering) return;
    finishDiscovery();
}

void CommManager::finishDiscovery() {
    discovering = false;
    discoveryChannels.clear();
    discoveryChannelIndex = 0;
    discoveryChannelUntil = 0;
    channelManager.applyStoredChannel();
    std::sort(discovered.begin(), discovered.end(), [](const DiscoveredDevice& a, const DiscoveredDevice& b){ return a.rssi > b.rssi; });
}

void CommManager::addOrUpdateDiscovered(const uint8_t mac[6], const char* name, int8_t rssi, float ton, float toff, uint8_t channel) {
    for (auto &d : discovered) {
        if (memcmp(d.mac, mac, 6) == 0) {
            d.rssi = rssi; d.ton = ton; d.toff = toff; d.lastSeen = millis(); d.channel = channel;
            if (name && *name) { strncpy(d.name, name, sizeof(d.name)-1); }
            return;
        }
    }
    DiscoveredDevice nd = {};
    memcpy(nd.mac, mac, 6);
    if (name && *name) strncpy(nd.name, name, sizeof(nd.name)-1);
    nd.rssi = rssi; nd.ton = ton; nd.toff = toff; nd.lastSeen = millis(); nd.channel = channel;
    discovered.push_back(nd);
}

void CommManager::pairWithIndex(int idx) {
    if (idx < 0 || idx >= (int)discovered.size()) return;
    const DiscoveredDevice &d = discovered[idx];
    // Add to device manager if not present
    if (deviceManager.findDeviceByMac(d.mac) < 0) {
        SlaveDevice dev = {};
        memcpy(dev.mac, d.mac, 6);
        strncpy(dev.name, d.name, sizeof(dev.name)-1);
        dev.ton = d.ton; dev.toff = d.toff; dev.outputState = false;
        dev.rssiRemote = d.rssi; dev.rssiSlave = d.rssi;
        deviceManager.addDevice(dev);
        if (deviceManager.getActiveIndex() < 0) {
            deviceManager.setActiveIndex(deviceManager.getDeviceCount()-1);
        }
    }
    // Temporarily hop to the discovered channel to talk to the timer
    channelManager.applyChannel(d.channel);
    // Send a follow-up PAIR (status request) directly to ensure state
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::PAIR;
    sendProtocol(d.mac, msg, "PAIR", true, cmdContext(ProtocolCmd::PAIR));
    sendChannelUpdate(d.mac);
    channelManager.applyStoredChannel();
    int deviceIdx = deviceManager.findDeviceByMac(d.mac);
    if (deviceIdx >= 0) {
        requestStatus(deviceManager.getDevice(deviceIdx));
    }
    if (discovering && !discoveryChannels.empty()) {
        switchDiscoveryChannel(discoveryChannels[discoveryChannelIndex]);
    }
}

bool CommManager::isDuplicateStatus(const uint8_t mac[6], float ton, float toff, bool state, unsigned long now) {
    for (auto &e : lastStatus) {
        if (memcmp(e.mac, mac, 6)==0) {
            if (e.ton==ton && e.toff==toff && e.state==state && (now - e.ts) < 150) return true;
            e.ton=ton; e.toff=toff; e.state=state; e.ts=now; return false;
        }
    }
    LastStatusCache c={}; memcpy(c.mac, mac,6); c.ton=ton; c.toff=toff; c.state=state; c.ts=now; lastStatus.push_back(c); return false;
}

void CommManager::ensurePeer(const uint8_t mac[6]) {
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p = {};
        memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
        p.encrypt = false;
        esp_err_t ar = esp_now_add_peer(&p);
        Serial.printf("[COMM] Added peer %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], (int)ar);
    }
}

void CommManager::setActiveName(const char* newName) {
    const SlaveDevice* act = deviceManager.getActive(); if (!act) return;
    ProtocolMsg msg={};
    msg.cmd = (uint8_t)ProtocolCmd::SET_NAME;
    strncpy(msg.name, newName, sizeof(msg.name)-1);
    sendProtocol(act->mac, msg, "SET_NAME", true, cmdContext(ProtocolCmd::SET_NAME));
    // Update local device name immediately for consistent UI reflection
    int idx = deviceManager.getActiveIndex();
    if (idx >= 0) {
        // Persist rename so it survives remote reboot
        deviceManager.renameDevice(idx, newName);
    }
}

void CommManager::setActiveTimer(float tonSec, float toffSec) {
    const SlaveDevice* act = deviceManager.getActive(); if (!act) return;
    ProtocolMsg msg={};
    msg.cmd = (uint8_t)ProtocolCmd::SET_TIMER;
    msg.ton = tonSec;
    msg.toff = toffSec;
    sendProtocol(act->mac, msg, "SET_TIMER", true, cmdContext(ProtocolCmd::SET_TIMER));
    Serial.printf("[COMM] Queue SET_TIMER %.1f/%.1f for %02X:%02X:%02X:%02X:%02X:%02X\n",
                  tonSec, toffSec, act->mac[0],act->mac[1],act->mac[2],act->mac[3],act->mac[4],act->mac[5]);
    requestStatus(*act);
}

void CommManager::sendChannelUpdate(const uint8_t mac[6]) {
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::SET_CHANNEL;
    msg.channel = channelManager.getStoredChannel();
    sendProtocol(mac, msg, "SET_CHANNEL", true, cmdContext(ProtocolCmd::SET_CHANNEL));
}

void CommManager::switchDiscoveryChannel(uint8_t channel) {
    channelManager.applyChannel(channel);
    discoveryChannelUntil = millis() + DISCOVERY_DWELL_MS;
    lastDiscoveryPing = millis();
    broadcastDiscovery();
}

void CommManager::onChannelChanged(uint8_t previousChannel) {
    uint8_t desired = channelManager.getStoredChannel();
    if (desired < 1 || desired > 13) {
        channelManager.applyStoredChannel();
        return;
    }
    if (previousChannel == desired) {
        channelManager.applyStoredChannel();
        return;
    }
    if (previousChannel >= 1 && previousChannel <= 13 && previousChannel != desired) {
        channelManager.applyChannel(previousChannel);
    }
    int count = deviceManager.getDeviceCount();
    for (int i = 0; i < count; ++i) {
        const SlaveDevice& dev = deviceManager.getDevice(i);
        sendChannelUpdate(dev.mac);
    }
    channelManager.applyStoredChannel();
    for (int i = 0; i < count; ++i) {
        const SlaveDevice& dev = deviceManager.getDevice(i);
        requestStatus(dev);
    }
}

void CommManager::factoryResetActive() {
    const SlaveDevice* act = deviceManager.getActive(); if (!act) return;
    ProtocolMsg msg={};
    msg.cmd = (uint8_t)ProtocolCmd::FACTORY_RESET;
    sendProtocol(act->mac, msg, "FACTORY_RESET", true, cmdContext(ProtocolCmd::FACTORY_RESET));
    requestStatus(*act);
}

void CommManager::renameDeviceByIndex(int idx, const char* newName) {
    if (idx < 0 || idx >= deviceManager.getDeviceCount() || !newName) {
        return;
    }
    char trimmed[sizeof(SlaveDevice::name)] = {};
    strncpy(trimmed, newName, sizeof(trimmed) - 1);
    deviceManager.renameDevice(idx, trimmed);
    const SlaveDevice& updated = deviceManager.getDevice(idx);
    ProtocolMsg msg = {};
    msg.cmd = static_cast<uint8_t>(ProtocolCmd::SET_NAME);
    strncpy(msg.name, trimmed, sizeof(msg.name) - 1);
    msg.name[sizeof(msg.name) - 1] = '\0';
    sendProtocol(updated.mac, msg, "SET_NAME", true, cmdContext(ProtocolCmd::SET_NAME));
    requestStatus(updated);
}

bool CommManager::programTimerByIndex(int idx, float tonSec, float toffSec) {
    if (idx < 0 || idx >= deviceManager.getDeviceCount()) {
        return false;
    }
    const SlaveDevice& dev = deviceManager.getDevice(idx);
    ProtocolMsg msg = {};
    msg.cmd = static_cast<uint8_t>(ProtocolCmd::SET_TIMER);
    msg.ton = tonSec;
    msg.toff = toffSec;
    bool queued = sendProtocol(dev.mac, msg, "SET_TIMER-PC", true, cmdContext(ProtocolCmd::SET_TIMER));
    if (queued) {
        requestStatus(dev);
    }
    return queued;
}

bool CommManager::setOverrideStateByIndex(int idx, bool on) {
    if (idx < 0 || idx >= deviceManager.getDeviceCount()) {
        return false;
    }
    const SlaveDevice& dev = deviceManager.getDevice(idx);
    ProtocolMsg msg = {};
    msg.cmd = static_cast<uint8_t>(ProtocolCmd::OVERRIDE_OUTPUT);
    msg.outputOverride = on;
    bool queued = sendProtocol(dev.mac, msg, "OVERRIDE-PC", true, cmdContext(ProtocolCmd::OVERRIDE_OUTPUT));
    if (queued) {
        requestStatus(dev);
    }
    return queued;
}

bool CommManager::sendProtocol(const uint8_t* mac, ProtocolMsg& msg, const char* tag, bool requireAck, void* context) {
    if (!mac) return false;
    if (msg.channel == 0) {
        msg.channel = channelManager.getStoredChannel();
    }
    ReliableProtocol::SendConfig cfg;
    cfg.requireAck = requireAck;
    cfg.retryIntervalMs = Defaults::COMM_RETRY_INTERVAL_MS;
    cfg.maxAttempts = Defaults::COMM_MAX_RETRIES;
    cfg.tag = tag;
    cfg.userContext = context;
    bool queued = reliableLink.sendStruct(mac, msg, cfg);
    if (!queued) {
        Serial.printf("[COMM] Failed to queue %s for %02X:%02X:%02X:%02X:%02X:%02X\n",
                      tag ? tag : cmdToString(static_cast<ProtocolCmd>(msg.cmd)),
                      mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }
    return queued;
}

bool CommManager::sendDebugPacket(const uint8_t* mac, const DebugProtocol::Packet& packet, const ReliableProtocol::SendConfig& cfg) {
    if (!mac) return false;
    DebugProtocol::Packet copy = packet;
    ReliableProtocol::SendConfig config = cfg;
    if (!config.tag) {
        config.tag = "DEBUG";
    }
    bool queued = reliableLink.sendStruct(mac, copy, config);
    if (!queued) {
        Serial.printf("[COMM] Failed to queue DEBUG for %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }
    return queued;
}

ReliableProtocol::HandlerResult CommManager::handleDebugPacket(const uint8_t* mac, const DebugProtocol::Packet& packet) {
    ReliableProtocol::HandlerResult result;
    if (debugBridge) {
        debugBridge->handleTimerPacket(mac, packet);
    }
    return result;
}

void CommManager::handleAck(const uint8_t* mac, ReliableProtocol::AckType type, uint8_t status, void* context, const char* tag) {
    const ProtocolCmd cmd = contextToCmd(context);
    const char* label = tag ? tag : cmdToString(cmd);
    const char* transportStatus = ReliableProtocol::statusToString(status);
    const char* protocolStatus = statusToString(static_cast<ProtocolStatus>(status));
    const char* statusText = transportStatus ? transportStatus : protocolStatus;
    if (!statusText) statusText = "-";

    switch (type) {
        case ReliableProtocol::AckType::Ack:
            Serial.printf("[COMM] ACK %s (%s) status=%u (%s) from %02X:%02X:%02X:%02X:%02X:%02X\n",
                          label,
                          cmdToString(cmd),
                          status,
                          statusText,
                          mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
            break;
        case ReliableProtocol::AckType::Nak:
            Serial.printf("[COMM] NAK %s (%s) status=%u (%s) from %02X:%02X:%02X:%02X:%02X:%02X\n",
                          label,
                          cmdToString(cmd),
                          status,
                          statusText,
                          mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
            break;
        case ReliableProtocol::AckType::Timeout:
            Serial.printf("[COMM] TIMEOUT %s (%s) status=%u (%s) from %02X:%02X:%02X:%02X:%02X:%02X\n",
                          label,
                          cmdToString(cmd),
                          status,
                          statusText,
                          mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
            break;
    }
}

void CommManager::processIncoming() {
    reliableLink.loop();
}

void CommManager::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!instance) return;
    instance->reliableLink.onReceive(mac, data, len);
}
