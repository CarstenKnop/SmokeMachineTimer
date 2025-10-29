// EspNowComm.cpp
// Handles ESP-NOW communication and protocol command processing.
#include "EspNowComm.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>


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
    // Enable promiscuous mode to read RSSI of incoming frames
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&EspNowComm::wifiSniffer);
}

void EspNowComm::loop() {
    // Push status to the last known sender when the output state changes
    pushStatusIfStateChanged();
}

int8_t EspNowComm::getRssi() const {
    return WiFi.RSSI();
}

void EspNowComm::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < (int)sizeof(ProtocolMsg)) return;
    ProtocolMsg msg;
    memcpy(&msg, data, sizeof(msg));
    Serial.printf("[SLAVE] RX %s seq=%u from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  cmdToString(static_cast<ProtocolCmd>(msg.cmd)),
                  msg.seq,
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    // Track last sender MAC for associating RSSI from sniffer
    memcpy(lastSenderMac, mac, 6);
        if (instance) instance->processCommand(msg, mac);
}

void EspNowComm::sendStatus(const uint8_t* mac, uint8_t seq) {
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
    reply.seq = seq;
    reply.refCmd = (uint8_t)ProtocolCmd::STATUS;
    reply.status = (uint8_t)ProtocolStatus::OK;
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p = {}; memcpy(p.peer_addr, mac, 6); p.channel = 0; p.encrypt = false; esp_now_add_peer(&p);
        Serial.printf("[SLAVE] Added peer for status %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }
    esp_err_t r = esp_now_send(mac, (uint8_t*)&reply, sizeof(reply));
    Serial.printf("[SLAVE] Sent STATUS seq=%u (%d)\n", reply.seq, (int)r);
}

void EspNowComm::sendAck(const uint8_t* mac, uint8_t seq, ProtocolCmd refCmd, ProtocolStatus status) {
    if (!seq) return;
    ProtocolMsg ack = {};
    ack.cmd = (status == ProtocolStatus::OK) ? (uint8_t)ProtocolCmd::ACK : (uint8_t)ProtocolCmd::NAK;
    ack.channel = channelSettings.getChannel();
    ack.seq = seq;
    ack.refCmd = static_cast<uint8_t>(refCmd);
    ack.status = static_cast<uint8_t>(status);
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p = {}; memcpy(p.peer_addr, mac, 6); p.channel = 0; p.encrypt = false; esp_now_add_peer(&p);
        Serial.printf("[SLAVE] Added peer for ack %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }
    esp_err_t r = esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
    Serial.printf("[SLAVE] Sent %s seq=%u ref=%s status=%s (%d)\n",
                  status == ProtocolStatus::OK ? "ACK" : "NAK",
                  seq,
                  cmdToString(refCmd),
                  statusToString(status),
                  (int)r);
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

void EspNowComm::processCommand(const ProtocolMsg& msg, const uint8_t* mac) {
    switch ((ProtocolCmd)msg.cmd) {
        case ProtocolCmd::PAIR:
            Serial.println("[SLAVE] PAIR -> sending STATUS");
            sendAck(mac, msg.seq, ProtocolCmd::PAIR, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        case ProtocolCmd::SET_TIMER:
            config.saveTimer(msg.ton, msg.toff);
            timer.setTimes(msg.ton, msg.toff);
            sendAck(mac, msg.seq, ProtocolCmd::SET_TIMER, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        case ProtocolCmd::OVERRIDE_OUTPUT:
            timer.overrideOutput(msg.outputOverride);
            sendAck(mac, msg.seq, ProtocolCmd::OVERRIDE_OUTPUT, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        case ProtocolCmd::RESET_STATE:
            timer.resetState();
            sendAck(mac, msg.seq, ProtocolCmd::RESET_STATE, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        case ProtocolCmd::TOGGLE_STATE:
            timer.toggleAndReset();
            sendAck(mac, msg.seq, ProtocolCmd::TOGGLE_STATE, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        case ProtocolCmd::SET_NAME:
            config.saveName(msg.name);
            sendAck(mac, msg.seq, ProtocolCmd::SET_NAME, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        case ProtocolCmd::SET_CHANNEL:
            if (channelSettings.isChannelSupported(msg.channel)) {
                if (!channelSettings.setChannel(msg.channel)) {
                    channelSettings.apply();
                }
                sendAck(mac, msg.seq, ProtocolCmd::SET_CHANNEL, ProtocolStatus::OK);
                sendStatus(mac, msg.seq);
            } else {
                sendAck(mac, msg.seq, ProtocolCmd::SET_CHANNEL, ProtocolStatus::INVALID_PARAM);
            }
            break;
        case ProtocolCmd::FACTORY_RESET:
            Serial.println("[SLAVE] FACTORY_RESET -> wiping EEPROM and restoring defaults");
            config.factoryReset();
            timer.setTimes(config.getTon(), config.getToff());
            channelSettings.resetToDefault();
            sendAck(mac, msg.seq, ProtocolCmd::FACTORY_RESET, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        case ProtocolCmd::GET_RSSI:
            sendAck(mac, msg.seq, ProtocolCmd::GET_RSSI, ProtocolStatus::OK);
            sendStatus(mac, msg.seq);
            break;
        default:
            sendAck(mac, msg.seq, static_cast<ProtocolCmd>(msg.cmd), ProtocolStatus::UNKNOWN_CMD);
            break;
    }
}

void EspNowComm::pushStatusIfStateChanged() {
    if (!instance) return;
    if (instance->timer.consumeStateChanged()) {
        // We don't have the remote MAC here; for simplicity, broadcast the status so the active remote can catch it.
        uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        ProtocolMsg reply = {};
        reply.cmd = (uint8_t)ProtocolCmd::STATUS;
        reply.ton = instance->config.getTon();
        reply.toff = instance->config.getToff();
        reply.elapsed = instance->timer.getCurrentStateSeconds();
        strncpy(reply.name, instance->config.getName(), sizeof(reply.name)-1);
        reply.outputOverride = instance->timer.isOutputOn();
        reply.resetState = false;
        reply.rssiAtTimer = instance->getRssi();
        reply.channel = instance->channelSettings.getChannel();
        reply.seq = 0;
        reply.refCmd = (uint8_t)ProtocolCmd::STATUS;
        reply.status = (uint8_t)ProtocolStatus::OK;
        if (!esp_now_is_peer_exist(broadcast)) {
            esp_now_peer_info_t p = {}; memcpy(p.peer_addr, broadcast, 6); p.channel = 0; p.encrypt = false; esp_now_add_peer(&p);
        }
        esp_now_send(broadcast, (uint8_t*)&reply, sizeof(reply));
    }
}
