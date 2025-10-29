// EspNowComm.h
// Handles ESP-NOW communication and protocol command processing.
#pragma once
#include <Arduino.h>
#include "protocol/Protocol.h"
#include "ReliableEspNow.h"
#include "ReliableProtocol.h"
#include "DebugProtocol.h"
#include "timer/TimerController.h"
#include "config/DeviceConfig.h"
#include "config/TimerChannelSettings.h"
#include <esp_wifi_types.h>

class EspNowComm {
public:
    EspNowComm(TimerController& timer, DeviceConfig& config, TimerChannelSettings& channelSettings);
    void begin();
    void loop();
    void pushStatusIfStateChanged();
    int8_t getRssi() const;
    static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);
private:
    TimerController& timer;
    DeviceConfig& config;
    TimerChannelSettings& channelSettings;
    void sendStatus(const uint8_t* mac, bool requireAck = true);
    ReliableProtocol::HandlerResult processCommand(const ProtocolMsg& msg, const uint8_t* mac);
    ReliableProtocol::HandlerResult handleFrame(const uint8_t* mac, const uint8_t* payload, size_t len);
    ReliableProtocol::HandlerResult handleDebugPacket(const uint8_t* mac, const DebugProtocol::Packet& packet);
    void ensurePeer(const uint8_t* mac);
    ReliableEspNow::Link reliableLink;
    static EspNowComm* instance;
    // RSSI capture via promiscuous callback
    static void wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type);
    static volatile int8_t lastRxRssi;
    static uint8_t lastSenderMac[6];
};
