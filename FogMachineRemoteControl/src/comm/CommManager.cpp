// CommManager.cpp
// Handles ESP-NOW communication and protocol command processing.
#include "CommManager.h"
#include <WiFi.h>
#include <esp_now.h>
#include <algorithm>
#include "protocol/Protocol.h"
#include "Pins.h"

// Status request helpers (reuse PAIR command as a lightweight status poll)
void CommManager::requestStatus(const SlaveDevice& dev) {
    ProtocolMsg msg = {}; msg.cmd = (uint8_t)ProtocolCmd::PAIR; // Interpret at slave as status request when already paired
    digitalWrite(COMM_OUT_GPIO, HIGH); ledBlinkUntil = millis() + 30;
    esp_now_send(dev.mac, (uint8_t*)&msg, sizeof(msg));
}

void CommManager::requestStatusActive() {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    requestStatus(*act);
}


CommManager* CommManager::instance = nullptr;


CommManager::CommManager(DeviceManager& deviceMgr) : deviceManager(deviceMgr) {}

void CommManager::begin() {
    instance = this;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
    }
    esp_now_register_recv_cb(CommManager::onDataRecv);
}

void CommManager::loop() {
    // Non-blocking COMM LED blink
    if (ledBlinkUntil && millis() > ledBlinkUntil) {
    digitalWrite(COMM_OUT_GPIO, LOW);
        ledBlinkUntil = 0;
    }
    // Discovery ticking
    if (discovering) {
        uint32_t now = millis();
        if (now - lastDiscoveryPing > 1000) { // ping every second
            broadcastDiscovery();
            lastDiscoveryPing = now;
        }
        if (now >= discoveryEnd) {
            finishDiscovery();
        }
    }
}

void CommManager::sendCommand(const SlaveDevice& dev, uint8_t cmd, const void* payload, size_t payloadSize) {
    ProtocolMsg msg = {};
    msg.cmd = cmd;
    if (payload && payloadSize > 0) memcpy(&msg, payload, payloadSize);
    // Non-blocking COMM LED blink on send
    digitalWrite(COMM_OUT_GPIO, HIGH);
    ledBlinkUntil = millis() + 30;
    esp_now_send(dev.mac, (uint8_t*)&msg, sizeof(msg));
}

void CommManager::broadcastDiscovery() {
    uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::PAIR;
    // Non-blocking COMM LED blink on broadcast
    digitalWrite(COMM_OUT_GPIO, HIGH);
    ledBlinkUntil = millis() + 30;
    esp_now_send(broadcast, (uint8_t*)&msg, sizeof(msg));
}

void CommManager::processIncoming() {
    // Called from onDataRecv
}

void CommManager::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    // Non-blocking COMM LED blink on receive
    if (instance) {
    digitalWrite(COMM_OUT_GPIO, HIGH);
        instance->ledBlinkUntil = millis() + 30;
    }
    if (len < (int)sizeof(ProtocolMsg)) return;
    ProtocolMsg msg;
    memcpy(&msg, data, sizeof(msg));
    if (!instance) return;
    // For now we treat incoming STATUS (and PAIR replies) the same: update discovered or paired device.
    int8_t rssi = -70; // Placeholder (IDF <5 API); could be improved using custom recv info if available.
    // If discovering, collect in discovered list
    if (instance->discovering) {
        instance->addOrUpdateDiscovered(mac, msg.name, rssi, msg.ton, msg.toff);
    }
    // Update existing paired device if present (status fields only, avoid EEPROM write each packet)
    int idx = instance->deviceManager.findDeviceByMac(mac);
    if (idx >= 0) {
        SlaveDevice dev = instance->deviceManager.getDevice(idx);
        dev.ton = msg.ton;
        dev.toff = msg.toff;
        dev.outputState = msg.outputOverride;
        dev.rssiRemote = rssi;
        dev.rssiSlave = rssi; // placeholder
        if (msg.name[0]) { strncpy(dev.name, msg.name, sizeof(dev.name)-1); dev.name[sizeof(dev.name)-1] = '\0'; }
        dev.lastStatusMs = millis();
        instance->deviceManager.updateStatus(idx, dev);
    }
}

void CommManager::startDiscovery(uint32_t durationMs) {
    discovering = true;
    discoveryEnd = millis() + durationMs;
    lastDiscoveryPing = 0;
    discovered.clear();
    broadcastDiscovery();
}

void CommManager::stopDiscovery() {
    if (!discovering) return;
    finishDiscovery();
}

void CommManager::finishDiscovery() {
    discovering = false;
    std::sort(discovered.begin(), discovered.end(), [](const DiscoveredDevice& a, const DiscoveredDevice& b){ return a.rssi > b.rssi; });
}

void CommManager::addOrUpdateDiscovered(const uint8_t mac[6], const char* name, int8_t rssi, float ton, float toff) {
    for (auto &d : discovered) {
        if (memcmp(d.mac, mac, 6) == 0) {
            d.rssi = rssi; d.ton = ton; d.toff = toff; d.lastSeen = millis();
            if (name && *name) { strncpy(d.name, name, sizeof(d.name)-1); }
            return;
        }
    }
    DiscoveredDevice nd = {};
    memcpy(nd.mac, mac, 6);
    if (name && *name) strncpy(nd.name, name, sizeof(nd.name)-1);
    nd.rssi = rssi; nd.ton = ton; nd.toff = toff; nd.lastSeen = millis();
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
    }
    // Send a follow-up PAIR (status request) directly to ensure state
    ProtocolMsg msg = {}; msg.cmd = (uint8_t)ProtocolCmd::PAIR;
    esp_now_send(d.mac, (uint8_t*)&msg, sizeof(msg));
}
