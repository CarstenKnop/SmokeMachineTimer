// ButtonInput.h - four active-low buttons with time-based debouncing and one-shot events.
#pragma once

#include <Arduino.h>
#include "Defaults.h"

class ButtonInput {
public:
    ButtonInput(uint8_t upGpio, uint8_t downGpio, uint8_t hashGpio, uint8_t starGpio);
    void begin();
    void update();

    // One-shot edges (true for exactly one update() cycle)
    bool upPressed() const { return edge_[0]; }
    bool downPressed() const { return edge_[1]; }
    bool hashPressed() const { return edge_[2]; }
    bool starPressed() const { return edge_[3]; }
    bool hashLongEvent() const { return hashLongEvent_; }     // fires once when '#' has been held LONG_PRESS_MS
    bool hashReleased() const { return releaseEdge_[2]; }
    bool hashClick() const { return hashClick_; }             // '#' released after a SHORT press (no long-press event fired)
    bool starReleased() const { return releaseEdge_[3]; }

    // Levels
    bool upHeld() const { return state_[0]; }
    bool downHeld() const { return state_[1]; }
    bool hashHeld() const { return state_[2]; }
    bool starHeld() const { return state_[3]; }
    bool anyHeld() const { return state_[0] || state_[1] || state_[2] || state_[3]; }

    unsigned long hashHoldDuration() const { return state_[2] ? (millis() - pressStart_[2]) : 0; }
    unsigned long starHoldDuration() const { return state_[3] ? (millis() - pressStart_[3]) : 0; }
    unsigned long upHoldDuration() const { return state_[0] ? (millis() - pressStart_[0]) : 0; }

    // True if the button was already down when begin() ran (e.g. the key that woke us from deep sleep).
    bool wasDownAtBoot(int idx) const { return bootDown_[idx]; }

    static constexpr unsigned long LONG_PRESS_MS = Defaults::BUTTON_LONG_PRESS_MS;

private:
    uint8_t pins_[4];
    bool state_[4] = {false, false, false, false};        // debounced level (true = pressed)
    bool raw_[4] = {false, false, false, false};
    bool edge_[4] = {false, false, false, false};
    bool releaseEdge_[4] = {false, false, false, false};
    bool bootDown_[4] = {false, false, false, false};
    bool swallow_[4] = {false, false, false, false};      // ignore this press until it is released
    uint32_t rawSince_[4] = {0, 0, 0, 0};
    uint32_t pressStart_[4] = {0, 0, 0, 0};
    bool hashLongSent_ = false;
    bool hashLongEvent_ = false;
    bool hashClick_ = false;
};
