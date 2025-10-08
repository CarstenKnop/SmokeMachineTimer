// EspNowComm.cpp
// Handles ESP-NOW communication and protocol command processing.
#include "EspNowComm.h"
#include <WiFi.h>
#include <esp_now.h>


EspNowComm* EspNowComm::instance = nullptr;


EspNowComm::EspNowComm(TimerController& timerRef, DeviceConfig& configRef)
    : timer(timerRef), config(configRef) {}

void EspNowComm::begin() {
    instance = this;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
    }
    esp_now_register_recv_cb(EspNowComm::onDataRecv);
}

void EspNowComm::loop() {
    // No periodic actions needed for slave
}

int8_t EspNowComm::getRssi() const {
    return WiFi.RSSI();
}

void EspNowComm::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < (int)sizeof(ProtocolMsg)) return;
    ProtocolMsg msg;
    memcpy(&msg, data, sizeof(msg));
        if (instance) instance->processCommand(msg, mac);
}

void EspNowComm::sendStatus(const uint8_t* mac) {
    ProtocolMsg reply = {};
    reply.cmd = (uint8_t)ProtocolCmd::STATUS;
    reply.ton = config.getTon();
    reply.toff = config.getToff();
    strncpy(reply.name, config.getName(), sizeof(reply.name)-1);
    reply.outputOverride = timer.isOutputOn();
    reply.resetState = false;
    esp_now_send(mac, (uint8_t*)&reply, sizeof(reply));
}

void EspNowComm::processCommand(const ProtocolMsg& msg, const uint8_t* mac) {
    switch ((ProtocolCmd)msg.cmd) {
        case ProtocolCmd::PAIR:
            sendStatus(mac);
            break;
        case ProtocolCmd::SET_TIMER:
            config.saveTimer(msg.ton, msg.toff);
            timer.setTimes(msg.ton, msg.toff);
            sendStatus(mac);
            break;
        case ProtocolCmd::OVERRIDE_OUTPUT:
            timer.overrideOutput(msg.outputOverride);
            sendStatus(mac);
            break;
        case ProtocolCmd::RESET_STATE:
            timer.resetState();
            sendStatus(mac);
            break;
        case ProtocolCmd::SET_NAME:
            config.saveName(msg.name);
            sendStatus(mac);
            break;
        case ProtocolCmd::GET_RSSI:
            sendStatus(mac);
            break;
        default:
            break;
    }
}
