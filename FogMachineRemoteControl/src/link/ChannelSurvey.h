// ChannelSurvey.h - measures how busy each 2.4 GHz channel is so the system can move to a quiet one.
//
// Two measurements per channel: (1) the Wi-Fi access points on it and their strength (active scan), and
// (2) the raw traffic heard during a short promiscuous dwell. Access points also interfere with their
// neighbours (Wi-Fi channels are 22 MHz wide), which the score takes into account.
#pragma once

#include <Arduino.h>
#include "FogProto.h"

class ChannelSurvey {
public:
    struct Channel {
        uint8_t aps = 0;
        int8_t apRssi = 0;       // strongest AP (dBm, 0 = none)
        uint16_t frames = 0;     // frames heard during the dwell
        int8_t avgRssi = 0;
        uint8_t score = 0;       // 0..100, lower = quieter
    };
    enum class State : uint8_t { Idle, Scanning, Dwell, Done, Failed };

    bool start(uint8_t restoreChannel, uint32_t nowMs);
    void tick(uint32_t nowMs);              // call every loop while active()
    void cancel();
    bool active() const { return state_ == State::Scanning || state_ == State::Dwell; }
    State state() const { return state_; }
    uint8_t progressPercent() const;
    const Channel& channel(uint8_t ch) const { return ch_[ch < 14 ? ch : 0]; }
    uint8_t bestCount() const { return bestCount_; }
    uint8_t best(uint8_t rank) const { return best_[rank]; }   // channel numbers, quietest first
    uint8_t restoreChannel() const { return restore_; }

private:
    void finishScan();
    void computeScores();
    void finish();

    State state_ = State::Idle;
    Channel ch_[14];
    uint8_t best_[Fog::kMaxChannel] = {0};
    uint8_t bestCount_ = 0;
    uint8_t restore_ = Fog::kDefaultChannel;
    uint8_t dwellCh_ = 0;
    uint32_t stateStart_ = 0;
};
