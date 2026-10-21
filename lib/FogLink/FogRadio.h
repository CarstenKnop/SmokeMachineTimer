// FogRadio.h - ESP-NOW transport for the FogLink protocol (ESP32, Arduino core 3.x / IDF 5.x).
//
// Everything that runs in the Wi-Fi task (receive / send callbacks) only copies data into queues or sets
// flags: no logging, no flash access, no allocation. The main loop drains the queues. That removes the
// data races of the previous design where protocol code ran inside the Wi-Fi callback.
#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include "FogProto.h"

namespace Fog {

class FogRadio {
public:
    struct Rx {
        uint8_t mac[6];
        int8_t rssi;
        uint8_t channel;         // channel the frame was received on
        uint8_t len;
        uint8_t data[kMaxFrame];
    };

    struct Stats {
        uint32_t tx = 0, txOk = 0, txFail = 0, txWd = 0, txDrop = 0;
        uint32_t rx = 0, rxDrop = 0;
        uint32_t peerErrors = 0;
    };

    // Brings up Wi-Fi in station mode (never connects), ESP-NOW and the given channel.
    bool begin(uint8_t channel, int8_t txPowerQdbm);
    void end();

    bool setChannel(uint8_t channel);
    uint8_t channel() const { return channel_; }
    void setTxPower(int8_t qdbm);
    void macAddress(uint8_t out[6]) const;

    // One frame in flight at a time. mac==nullptr -> broadcast. False: busy or error.
    bool send(const uint8_t* mac, const uint8_t* data, size_t len);
    bool txBusy() const { return txBusy_; }
    // Poll in loop(): true once per finished send. ok = the receiver's MAC acknowledged (broadcast: always true).
    bool pollTxResult(bool& ok);
    // Poll in loop(): pops one received frame.
    bool pollRx(Rx& out);

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats(); }

    // Call every loop: enforces the send-callback watchdog.
    void service(uint32_t nowMs);

private:
    static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
    static void onSend(const esp_now_send_info_t* info, esp_now_send_status_t status);
    bool ensurePeer(const uint8_t* mac);

    static FogRadio* self_;
    QueueHandle_t rxQueue_ = nullptr;
    volatile bool txBusy_ = false;
    volatile bool txDone_ = false;
    volatile bool txOk_ = false;
    uint32_t txStartMs_ = 0;
    uint8_t channel_ = kDefaultChannel;
    bool up_ = false;
    Stats stats_;
    uint8_t peers_[12][6] = {};
    uint8_t peerCount_ = 0;
    uint8_t peerNext_ = 0;
};

}  // namespace Fog
