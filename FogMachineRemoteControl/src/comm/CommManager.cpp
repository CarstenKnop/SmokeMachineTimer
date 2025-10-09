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

// Status request helpers (reuse PAIR command as a lightweight status poll)
void CommManager::requestStatus(const SlaveDevice& dev) {
    ProtocolMsg msg = {}; msg.cmd = (uint8_t)ProtocolCmd::PAIR; // Interpret at slave as status request when already paired
    if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW); ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    ensurePeer(dev.mac);
    esp_err_t r = esp_now_send(dev.mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[COMM] Sent STATUS-REQ to %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", dev.mac[0],dev.mac[1],dev.mac[2],dev.mac[3],dev.mac[4],dev.mac[5], (int)r);
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
    ensurePeer(act->mac);
           if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
        ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    esp_err_t r = esp_now_send(act->mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[COMM] Sent RESET to %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", act->mac[0],act->mac[1],act->mac[2],act->mac[3],act->mac[4],act->mac[5], (int)r);
    requestStatus(*act);
}

void CommManager::toggleActive() {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::TOGGLE_STATE;
    ensurePeer(act->mac);
           if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
        ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    esp_err_t r = esp_now_send(act->mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[COMM] Sent TOGGLE to %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", act->mac[0],act->mac[1],act->mac[2],act->mac[3],act->mac[4],act->mac[5], (int)r);
    requestStatus(*act);
}

void CommManager::overrideActive(bool on) {
    const SlaveDevice* act = deviceManager.getActive();
    if (!act) return;
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::OVERRIDE_OUTPUT;
    msg.outputOverride = on;
    ensurePeer(act->mac);
    if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
    ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    esp_now_send(act->mac, (uint8_t*)&msg, sizeof(msg));
}


CommManager* CommManager::instance = nullptr;


CommManager::CommManager(DeviceManager& deviceMgr) : deviceManager(deviceMgr) {}

void CommManager::begin() {
    instance = this;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Force channel 1 (must match slave) – adjust if hardware uses different channel
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
    }
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
    // Non-blocking COMM LED blink
    if (ledBlinkUntil && millis() > ledBlinkUntil) {
        if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, LOW); else digitalWrite(COMM_OUT_GPIO, HIGH);
        ledBlinkUntil = 0;
    }
    // Discovery ticking
    if (discovering) {
        uint32_t now = millis();
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
    if (payload && payloadSize > 0) memcpy(&msg, payload, payloadSize);
    // Non-blocking COMM LED blink on send
    // Ensure peer exists before sending
    ensurePeer(dev.mac);
           if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
        ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    esp_err_t r = esp_now_send(dev.mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[COMM] Sent cmd=%u to %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", (unsigned)cmd, dev.mac[0],dev.mac[1],dev.mac[2],dev.mac[3],dev.mac[4],dev.mac[5], (int)r);
}

void CommManager::broadcastDiscovery() {
    uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ProtocolMsg msg = {};
    msg.cmd = (uint8_t)ProtocolCmd::PAIR;
    // Non-blocking COMM LED blink on broadcast
    if (!esp_now_is_peer_exist(broadcast)) {
        esp_now_peer_info_t p = {}; memcpy(p.peer_addr, broadcast, 6); p.channel = 1; p.encrypt = false; esp_now_add_peer(&p);
        Serial.println("[COMM] Added broadcast peer");
    }
        if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
        ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    esp_err_t r = esp_now_send(broadcast, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[DISC] Broadcast PAIR sent (%d)\n", (int)r);
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
    Serial.printf("[COMM] RX cmd=%u from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n", (unsigned)msg.cmd, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], len);
    if (!instance) return;
    // For now we treat incoming STATUS (and PAIR replies) the same: update discovered or paired device.
    int8_t rssi = -70; // TODO: Replace with real RSSI if available from RX metadata
    // If discovering, collect in discovered list
    if (instance->discovering) {
        instance->addOrUpdateDiscovered(mac, msg.name, rssi, msg.ton, msg.toff);
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
    // RSSI robustness: treat -127 as invalid; keep last-good for a grace window handled by UI; still store value
            // Update timer-side RSSI only if valid (avoid overwriting with -127 blips)
            if (msg.rssiAtTimer > -120) {
                dev.rssiSlave = msg.rssiAtTimer; // value measured at timer
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
        if (deviceManager.getActiveIndex() < 0) {
            deviceManager.setActiveIndex(deviceManager.getDeviceCount()-1);
        }
    }
    // Send a follow-up PAIR (status request) directly to ensure state
    ProtocolMsg msg = {}; msg.cmd = (uint8_t)ProtocolCmd::PAIR;
    ensurePeer(d.mac);
    if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
    ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    esp_err_t r = esp_now_send(d.mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[COMM] Sent STATUS-REQ after pair to %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", d.mac[0],d.mac[1],d.mac[2],d.mac[3],d.mac[4],d.mac[5], (int)r);
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
        p.channel = 1;
        p.encrypt = false;
        esp_err_t ar = esp_now_add_peer(&p);
        Serial.printf("[COMM] Added peer %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], (int)ar);
    }
}

void CommManager::setActiveName(const char* newName) {
    const SlaveDevice* act = deviceManager.getActive(); if (!act) return;
    ProtocolMsg msg={}; msg.cmd = (uint8_t)ProtocolCmd::SET_NAME; strncpy(msg.name, newName, sizeof(msg.name)-1);
    esp_now_send(act->mac, (uint8_t*)&msg, sizeof(msg));
    // Update local device name immediately for consistent UI reflection
    int idx = deviceManager.getActiveIndex();
    if (idx >= 0) {
        // Persist rename so it survives remote reboot
        deviceManager.renameDevice(idx, newName);
    }
}

void CommManager::setActiveTimer(float tonSec, float toffSec) {
    const SlaveDevice* act = deviceManager.getActive(); if (!act) return;
    ProtocolMsg msg={}; msg.cmd = (uint8_t)ProtocolCmd::SET_TIMER; msg.ton = tonSec; msg.toff = toffSec;
    ensurePeer(act->mac);
    esp_err_t r = esp_now_send(act->mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[COMM] Sent SET_TIMER to %02X:%02X:%02X:%02X:%02X:%02X (%.1f/%.1f) (%d)\n", act->mac[0],act->mac[1],act->mac[2],act->mac[3],act->mac[4],act->mac[5], tonSec, toffSec, (int)r);
    requestStatus(*act);
}

void CommManager::factoryResetActive() {
    const SlaveDevice* act = deviceManager.getActive(); if (!act) return;
    ProtocolMsg msg={}; msg.cmd = (uint8_t)ProtocolCmd::FACTORY_RESET;
    ensurePeer(act->mac);
    if (Defaults::COMM_LED_ACTIVE_HIGH) digitalWrite(COMM_OUT_GPIO, HIGH); else digitalWrite(COMM_OUT_GPIO, LOW);
    ledBlinkUntil = millis() + Defaults::COMM_LED_MIN_ON_MS;
    esp_err_t r = esp_now_send(act->mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[COMM] Sent FACTORY_RESET to %02X:%02X:%02X:%02X:%02X:%02X (%d)\n", act->mac[0],act->mac[1],act->mac[2],act->mac[3],act->mac[4],act->mac[5], (int)r);
    requestStatus(*act);
}
