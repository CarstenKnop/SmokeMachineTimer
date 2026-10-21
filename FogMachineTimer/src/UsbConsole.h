// UsbConsole.h - FogSerial/2 endpoint of a timer plugged straight into the PC (docs/SERIAL_PROTOCOL.md).
// Lets the Fog Console configure and debug a single smoke machine without a remote.
#pragma once

#include <Arduino.h>
#include "EspTimerPlatform.h"
#include "FogJsonLine.h"
#include "FogLog.h"

class UsbConsole : public Fog::OtaUsbSink {
public:
    UsbConsole(Fog::TimerNode& node, EspTimerPlatform& platform) : node_(node), plat_(platform) {}

    void begin();
    void loop(uint32_t nowMs);
    bool pcPresent(uint32_t nowMs) const { return json_.pcPresent(nowMs); }
    // Reads from Serial; shared with main so a stray boot text does not matter.
    void onFrame(bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len, uint32_t nowMs);
    // Fog::OtaUsbSink: a reply of the update receiver to a transfer that came over USB
    void onOtaReply(const uint8_t* payload, size_t len) override;

private:
    void handle(JsonDocument& doc, uint32_t nowMs);
    void handleCmd(JsonDocument& doc, uint32_t id, uint32_t nowMs);
    void handleOtaTx(JsonDocument& doc, uint32_t id, uint32_t nowMs);
    void sendFleet(uint32_t nowMs);
    void sendRemote(uint32_t nowMs);
    void sendStats(uint32_t nowMs, uint32_t id, bool asResponse);
    void drainLog();
    void fillSettings(JsonObject o) const;

    Fog::TimerNode& node_;
    EspTimerPlatform& plat_;
    Fog::JsonLine json_;
    Fog::LogLevel logLevel_ = Fog::LogLevel::Off;
    bool sniff_ = false;
    uint32_t statsEveryMs_ = 0;
    uint32_t lastFleetMs_ = 0, lastRemoteMs_ = 0, lastStatsMs_ = 0, lastSniffMs_ = 0;
    uint32_t seq_ = 0;
    uint32_t sniffBudget_ = 0;
    uint32_t sniffWindowMs_ = 0;
};
