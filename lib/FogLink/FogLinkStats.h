// FogLinkStats.h - small, allocation-free helpers used to judge link health.
#pragma once

#include <stdint.h>
#include <string.h>

namespace Fog {

// Tracks frame loss of one peer from the gaps in its 8-bit frame counter.
// A long silence (remote asleep, timer unpowered) resets the window instead of counting misses.
class LossTracker {
public:
    void reset() { have_ = false; bits_ = 0; count_ = 0; }
    // The receiver was deaf for a while (channel sweep, survey): do not count the frames it could not hear as lost.
    void resync() { have_ = false; }
    void resetTotals() { rxTotal_ = lostTotal_ = dupTotal_ = 0; }
    uint32_t rxTotal() const { return rxTotal_; }
    uint32_t lostTotal() const { return lostTotal_; }
    uint32_t dupTotal() const { return dupTotal_; }

    void onFrame(uint8_t seq, uint32_t nowMs) {
        ++rxTotal_;
        if (!have_) {                               // first frame (or right after resync()): no gap can be judged yet
            have_ = true;
            lastSeq_ = seq;
            lastMs_ = nowMs;
            push(true);
            return;
        }
        if (static_cast<uint32_t>(nowMs - lastMs_) > kGapResetMs) {
            reset();
            have_ = true;
            lastSeq_ = seq;
            lastMs_ = nowMs;
            push(true);
            return;
        }
        const uint8_t gap = static_cast<uint8_t>(seq - lastSeq_);
        lastMs_ = nowMs;
        if (gap == 0) { ++dupTotal_; --rxTotal_; return; }   // duplicate delivery, ignore
        if (gap > 16) { reset(); have_ = true; lastSeq_ = seq; push(true); return; }   // resync
        for (uint8_t i = 1; i < gap; ++i) { push(false); ++lostTotal_; }
        push(true);
        lastSeq_ = seq;
    }

    // Percent of frames lost in the last <=32 expected frames; -1 until enough data.
    int lossPercent() const {
        if (count_ < kMinSamples) return -1;
        const uint32_t mask = (count_ >= 32) ? 0xFFFFFFFFu : ((1u << count_) - 1u);
        const uint32_t received = popcount32(bits_ & mask);
        return static_cast<int>(((count_ - received) * 100u + count_ / 2) / count_);
    }
    uint8_t samples() const { return count_; }

private:
    static uint32_t popcount32(uint32_t v) { uint32_t n = 0; while (v) { v &= v - 1; ++n; } return n; }
    static constexpr uint32_t kGapResetMs = 4000;
    static constexpr uint8_t kMinSamples = 8;
    void push(bool received) {
        bits_ = (bits_ << 1) | (received ? 1u : 0u);
        if (count_ < 32) ++count_;
    }
    bool have_ = false;
    uint8_t lastSeq_ = 0;
    uint8_t count_ = 0;
    uint32_t bits_ = 0;
    uint32_t lastMs_ = 0;
    uint32_t rxTotal_ = 0, lostTotal_ = 0, dupTotal_ = 0;
};

// Integer exponential moving average of RSSI (dBm).
class RssiFilter {
public:
    void reset() { valid_ = false; }
    void add(int8_t rssi) {
        if (rssi >= 0 || rssi <= -120) return;       // ignore bogus readings
        if (!valid_) { avg8_ = rssi * 8; valid_ = true; return; }
        avg8_ += (rssi * 8 - avg8_) / 4;             // alpha = 1/4
    }
    int8_t value() const { return valid_ ? static_cast<int8_t>(avg8_ / 8) : 0; }
    bool valid() const { return valid_; }

private:
    bool valid_ = false;
    int32_t avg8_ = 0;
};

// Remembers the last command handled per sender so a retransmitted command is not executed twice.
class CmdDedup {
public:
    struct Hit { bool duplicate; uint8_t result; };

    Hit check(const uint8_t* src, uint8_t cmdId, uint32_t nowMs) const {
        for (const Entry& e : table_) {
            if (e.used && memcmp(e.src, src, 6) == 0) {
                const bool recent = static_cast<uint32_t>(nowMs - e.ms) < kWindowMs;
                if (recent && e.cmdId == cmdId) return {true, e.result};
                break;
            }
        }
        return {false, 0};
    }

    void record(const uint8_t* src, uint8_t cmdId, uint8_t result, uint32_t nowMs) {
        Entry* slot = nullptr;
        for (Entry& e : table_) if (e.used && memcmp(e.src, src, 6) == 0) { slot = &e; break; }
        if (!slot) {
            slot = &table_[0];
            for (Entry& e : table_) { if (!e.used) { slot = &e; break; } if (static_cast<uint32_t>(nowMs - e.ms) > static_cast<uint32_t>(nowMs - slot->ms)) slot = &e; }
        }
        slot->used = true;
        memcpy(slot->src, src, 6);
        slot->cmdId = cmdId;
        slot->result = result;
        slot->ms = nowMs;
    }

private:
    static constexpr uint32_t kWindowMs = 6000;
    struct Entry { bool used = false; uint8_t src[6] = {0}; uint8_t cmdId = 0; uint8_t result = 0; uint32_t ms = 0; };
    Entry table_[4];
};

}  // namespace Fog
