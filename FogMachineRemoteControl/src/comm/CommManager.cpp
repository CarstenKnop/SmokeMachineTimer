// CommManager.cpp
// Handles ESP-NOW communication and protocol command processing.
#include "CommManager.h"
#include <WiFi.h>
#include <esp_now.h>
#include <algorithm>
#include "protocol/Protocol.h"
#include "Pins.h"
#include <esp_wifi.h>
#include "Defaults.h"
#include <esp_wifi_types.h>
#include "channel/RemoteChannelManager.h"

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
        case ProtocolCmd::ACK: return "ACK";
        case ProtocolCmd::NAK: return "NAK";
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
}

// Status request helpers (reuse PAIR command as a lightweight status poll)
void CommManager::requestStatus(const SlaveDevice& dev) {
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::PAIR; // interpret as status poll when already paired
    queueMessage(dev.mac, msg, true, "STATUS-REQ");
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
    queueMessage(act->mac, msg, true, "RESET");
    requestStatus(*act);
}

void CommManager::toggleActive() {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::TOGGLE_STATE;
    queueMessage(act->mac, msg, true, "TOGGLE");
    requestStatus(*act);
}

void CommManager::overrideActive(bool on) {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::OVERRIDE_OUTPUT;
    msg.outputOverride = on;
    queueMessage(act->mac, msg, true, "OVERRIDE");
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
    servicePendingTx();
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

void CommManager::sendCommand(const SlaveDevice& dev, uint8_t cmd, const void* payload, size_t payloadSize) {
    ProtocolMsg msg = {};
    msg.cmd = cmd;
    if (payload && payloadSize > 0) {
        size_t copyLen = std::min(payloadSize, sizeof(ProtocolMsg) - sizeof(msg.cmd));
        memcpy(reinterpret_cast<uint8_t*>(&msg) + sizeof(msg.cmd), payload, copyLen);
    }
    queueMessage(dev.mac, msg, true);
}

void CommManager::broadcastDiscovery() {
    uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::PAIR;
    if (!esp_now_is_peer_exist(broadcast)) {
        esp_now_peer_info_t p = {}; memcpy(p.peer_addr, broadcast, 6); p.channel = 0; p.encrypt = false; esp_now_add_peer(&p);
        Serial.println("[COMM] Added broadcast peer");
    }
    queueMessage(broadcast, msg, false, "DISCOVERY");
}

void CommManager::processIncoming() {
    // Called from onDataRecv
}

void CommManager::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    // Non-blocking COMM LED blink on receive
    if (instance) {
        if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
            instance->ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    }
    // Copy with exact current protocol size (no backward-compat per request)
    ProtocolMsg msg = {};
    if (len < (int)sizeof(ProtocolMsg)) { Serial.println("[COMM] Dropping short packet"); return; }
    memcpy(&msg, data, sizeof(ProtocolMsg));
    ProtocolCmd cmd = static_cast<ProtocolCmd>(msg.cmd);
    if (cmd == ProtocolCmd::ACK || cmd == ProtocolCmd::NAK) {
        if (instance) instance->handleAck(mac, msg);
        return;
    }
    Serial.printf("[COMM] RX %s seq=%u from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
                  cmdToString(cmd), msg.seq, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], len);
    if (!instance) return;
    // For now we treat incoming STATUS (and PAIR replies) the same: update discovered or paired device.
    int8_t rssi = -70; // TODO: Replace with real RSSI if available from RX metadata
    uint8_t reportedChannel = msg.channel;
    if (reportedChannel < 1 || reportedChannel > 13) {
        reportedChannel = instance->channelManager.getActiveChannel();
    }
    // If discovering, collect in discovered list
    if (instance->discovering) {
        instance->addOrUpdateDiscovered(mac, msg.name, rssi, msg.ton, msg.toff, reportedChannel);
    }
    // De-duplicate rapid identical STATUS packets (ton,toff,state) within 150ms
    if (msg.cmd == (uint8_t)ProtocolCmd::STATUS) {
        if (instance->isDuplicateStatus(mac, msg.ton, msg.toff, msg.outputOverride, millis())) {
            // even if duplicate, we could still refresh elapsed if desired; keep simple and drop
            return;
        }
    }
    // Update existing paired device if present (status fields only, avoid EEPROM write each packet)
    int idx = instance->deviceManager.findDeviceByMac(mac);
    if (idx >= 0) {
        SlaveDevice dev = instance->deviceManager.getDevice(idx);
        dev.ton = msg.ton;
        dev.toff = msg.toff;
    dev.outputState = msg.outputOverride;
    dev.elapsed = msg.elapsed;
    // Update RSSI estimates: remote side (placeholder) and timer-provided RSSI
    dev.rssiRemote = rssi; // TODO: replace with real RX RSSI if available
    // Normalize timer-side RSSI:
    // - Some firmwares report magnitude as positive (e.g., 40 for -40 dBm). Interpret >0 as negative.
    // - Treat 0 and <= -120 as invalid/sentinel.
    int8_t rssiTimer = msg.rssiAtTimer;
    if (rssiTimer > 0) rssiTimer = (int8_t)(-rssiTimer);
    if (rssiTimer < 0 && rssiTimer > -120) {
        dev.rssiSlave = rssiTimer; // value measured at timer (normalized)
    }
    // Reflect name
    if (msg.name[0]) { strncpy(dev.name, msg.name, sizeof(dev.name)-1); dev.name[sizeof(dev.name)-1] = '\0'; }
        dev.lastStatusMs = millis();
        instance->deviceManager.updateStatus(idx, dev);
    }
}

void CommManager::startDiscovery(uint32_t durationMs) {
    discovering = true;
    if (durationMs == 0) {
        discoveryEnd = UINT32_MAX; // run continuously until explicitly stopped
    } else {
        discoveryEnd = millis() + durationMs;
    }
    lastDiscoveryPing = 0;
    discovered.clear();
    discoveryChannels.clear();
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
    delay(15);
    // Send a follow-up PAIR (status request) directly to ensure state
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::PAIR;
    queueMessage(d.mac, msg, true, "PAIR");
    sendChannelUpdate(d.mac);
    delay(15);
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
    queueMessage(act->mac, msg, true, "SET_NAME");
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
    queueMessage(act->mac, msg, true, "SET_TIMER");
    Serial.printf("[COMM] Queue SET_TIMER %.1f/%.1f for %02X:%02X:%02X:%02X:%02X:%02X\n",
                  tonSec, toffSec, act->mac[0],act->mac[1],act->mac[2],act->mac[3],act->mac[4],act->mac[5]);
    requestStatus(*act);
}

void CommManager::sendChannelUpdate(const uint8_t mac[6]) {
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::SET_CHANNEL;
    msg.channel = channelManager.getStoredChannel();
    queueMessage(mac, msg, true);
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
        delay(15);
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
    queueMessage(act->mac, msg, true, "FACTORY_RESET");
    requestStatus(*act);
}

uint8_t CommManager::queueMessage(const uint8_t mac[6], ProtocolMsg& msg, bool expectAck, const char* label, uint8_t maxAttemptsOverride) {
    PendingTx pending = {};
    memcpy(pending.mac, mac, 6);
    pending.msg = msg;
    pending.label = label ? label : cmdToString(static_cast<ProtocolCmd>(msg.cmd));
    pending.expectAck = expectAck;
    pending.retryDelayMs = Defaults::COMM_RETRY_INTERVAL_MS ? Defaults::COMM_RETRY_INTERVAL_MS : 200;
    pending.maxAttempts = maxAttemptsOverride ? maxAttemptsOverride : Defaults::COMM_MAX_RETRIES;
    pending.attempts = 0;
    pending.lastSendMs = 0;
    if (expectAck) {
        pending.msg.seq = reserveSequence();
        pendingTx.push_back(pending);
        PendingTx& stored = pendingTx.back();
        sendPending(stored);
        return stored.msg.seq;
    } else {
        pending.msg.seq = 0;
        sendPending(pending);
        return 0;
    }
}

void CommManager::sendPending(PendingTx& tx) {
    tx.msg.channel = channelManager.getStoredChannel();
    commLedOn();
    unsigned long now = millis();
    ledBlinkUntil = now + Defaults::COMM_LED_MIN_ON_MS;
    ensurePeer(tx.mac);
    tx.lastSendMs = now;
    if (tx.attempts < 0xFF) {
        tx.attempts++;
    }
    esp_err_t r = esp_now_send(tx.mac, reinterpret_cast<uint8_t*>(&tx.msg), sizeof(ProtocolMsg));
    Serial.printf("[COMM] TX %s seq=%u attempt=%u -> %02X:%02X:%02X:%02X:%02X:%02X (%d)\n",
                  tx.label ? tx.label : cmdToString(static_cast<ProtocolCmd>(tx.msg.cmd)),
                  tx.msg.seq,
                  tx.attempts,
                  tx.mac[0],tx.mac[1],tx.mac[2],tx.mac[3],tx.mac[4],tx.mac[5],
                  (int)r);
}

void CommManager::servicePendingTx() {
    if (pendingTx.empty()) return;
    unsigned long now = millis();
    size_t i = 0;
    while (i < pendingTx.size()) {
        PendingTx& tx = pendingTx[i];
        if (!tx.expectAck) {
            pendingTx.erase(pendingTx.begin() + i);
            continue;
        }
        if (tx.maxAttempts && tx.attempts >= tx.maxAttempts) {
            Serial.printf("[COMM] Giving up %s seq=%u after %u attempts\n",
                          tx.label ? tx.label : cmdToString(static_cast<ProtocolCmd>(tx.msg.cmd)),
                          tx.msg.seq,
                          tx.attempts);
            pendingTx.erase(pendingTx.begin() + i);
            continue;
        }
        if (now - tx.lastSendMs >= tx.retryDelayMs) {
            sendPending(tx);
        }
        ++i;
    }
}

void CommManager::handleAck(const uint8_t* mac, const ProtocolMsg& msg) {
    bool success = static_cast<ProtocolCmd>(msg.cmd) == ProtocolCmd::ACK;
    const char* ackWord = success ? "ACK" : "NAK";
    uint8_t seq = msg.seq;
    if (!seq) {
        Serial.printf("[COMM] %s missing sequence from %02X:%02X:%02X:%02X:%02X:%02X\n",
                      ackWord, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        return;
    }
    for (size_t i = 0; i < pendingTx.size(); ++i) {
        PendingTx& tx = pendingTx[i];
        if (tx.msg.seq == seq && memcmp(tx.mac, mac, 6) == 0) {
            Serial.printf("[COMM] %s %s seq=%u status=%s after %u attempts from %02X:%02X:%02X:%02X:%02X:%02X\n",
                          ackWord,
                          tx.label ? tx.label : cmdToString(static_cast<ProtocolCmd>(tx.msg.cmd)),
                          seq,
                          statusToString(static_cast<ProtocolStatus>(msg.status)),
                          tx.attempts,
                          mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
            pendingTx.erase(pendingTx.begin() + i);
            return;
        }
    }
    Serial.printf("[COMM] %s seq=%u ref=%s status=%s (no pending match) from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  ackWord,
                  seq,
                  cmdToString(static_cast<ProtocolCmd>(msg.refCmd)),
                  statusToString(static_cast<ProtocolStatus>(msg.status)),
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

uint8_t CommManager::reserveSequence() {
    for (int attempt = 0; attempt < 255; ++attempt) {
        uint8_t candidate = nextSeq;
        nextSeq = (nextSeq == 255) ? 1 : static_cast<uint8_t>(nextSeq + 1);
        if (!sequenceInUse(candidate)) {
            return candidate;
        }
    }
    // Fallback if every sequence is somehow in use; reuse 1.
    return 1;
}

bool CommManager::sequenceInUse(uint8_t seq) const {
    if (!seq) return true;
    for (const auto& tx : pendingTx) {
        if (tx.msg.seq == seq) {
            return true;
        }
    }
    return false;
}
