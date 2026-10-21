// EspTimerPlatform.h - ESP32 implementation of Fog::TimerPlatform (radio queue, NVS, LED).
#pragma once

#include <Arduino.h>
#include <functional>
#include "FogRadio.h"
#include "FogStore.h"
#include "FogSys.h"
#include "FogTimerNode.h"

class EspTimerPlatform : public Fog::TimerPlatform {
public:
    // Frame tap for the PC packet inspector (tx = true for frames we send).
    using Tap = std::function<void(bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* data, size_t len)>;

    Fog::FogRadio radio;
    Fog::LifeCounters life;
    Fog::LoopMeter loop;
    uint8_t resetReason = 0;
    uint32_t txQueueDrops = 0;
    Tap tap;

    void begin(const Fog::TimerSettings& s, int8_t txPowerQdbm);
    // Moves frames between the radio and the node. Call every loop.
    void service(uint32_t nowMs, Fog::TimerNode& node);
    void updateLed(uint32_t nowMs, Fog::TimerNode& node, bool settingsLed);
    bool identifying(uint32_t nowMs) const { return identifyUntil_ && static_cast<int32_t>(identifyUntil_ - nowMs) > 0; }

    // Fog::TimerPlatform
    bool send(const uint8_t* mac, const uint8_t* data, size_t len) override;
    void setChannel(uint8_t ch) override { radio.setChannel(ch); }
    void saveSettings(const Fog::TimerSettings& s) override { store_.save(s); }
    void eraseSettings() override { store_.erase(); }
    void reboot() override { rebootAt_ = millis() + 50; }
    void identify(uint16_t ms) override { identifyUntil_ = millis() + ms; if (!identifyUntil_) identifyUntil_ = 1; }
    void fillInfo(Fog::InfoPayload& info) override;
    uint32_t random(uint32_t n) override { return n ? esp_random() % n : 0; }

    bool loadSettings(Fog::TimerSettings& out) const { return store_.load(out); }
    bool rebootDue() const { return rebootAt_ && static_cast<int32_t>(millis() - rebootAt_) >= 0; }

private:
    struct Queued { uint8_t mac[6]; bool unicast; uint8_t len; uint8_t data[Fog::kMaxFrame]; };
    static constexpr uint8_t kQueue = 6;
    Queued q_[kQueue];
    uint8_t qHead_ = 0, qTail_ = 0;
    bool inFlightUnicast_ = false;
    Fog::BlobStore<Fog::TimerSettings> store_{"fogtimer", "settings", 1};
    uint32_t identifyUntil_ = 0;
    uint32_t rebootAt_ = 0;
    uint32_t ledUntil_ = 0;
    uint32_t lastRxCount_ = 0;
};
