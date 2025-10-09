// EspNowComm.h
// Handles ESP-NOW communication and protocol command processing.
#pragma once
#include <Arduino.h>
#include "protocol/Protocol.h"
#include "timer/TimerController.h"
#include "config/DeviceConfig.h"

class EspNowComm {
public:
    EspNowComm(TimerController& timer, DeviceConfig& config);
    void begin();
    void loop();
    void pushStatusIfStateChanged();
    int8_t getRssi() const;
    static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);
private:
    TimerController& timer;
    DeviceConfig& config;
    void sendStatus(const uint8_t* mac);
    void processCommand(const ProtocolMsg& msg, const uint8_t* mac);
    static EspNowComm* instance;
};
