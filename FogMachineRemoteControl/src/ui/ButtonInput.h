// ButtonInput.h
// Handles button polling and debouncing.
#pragma once
#include <Arduino.h>
#include "Defaults.h"

class ButtonInput {
public:
    ButtonInput(uint8_t upGpio, uint8_t downGpio, uint8_t hashGpio, uint8_t starGpio);
    void begin();
    void update();
    bool upPressed() const { return edgeFlags[0]; }
    bool downPressed() const { return edgeFlags[1]; }
    bool leftPressed() const { return edgeFlags[2]; }   // legacy name for hash
    bool rightPressed() const { return edgeFlags[3]; }  // legacy name for star
    bool hashPressed() const { return edgeFlags[2]; }
    bool starPressed() const { return edgeFlags[3]; }
    bool hashLongPressed() const { return hashLongPressActive; }
    bool hashHeld() const { return states[2]; }
    bool upHeld() const { return states[0]; }
    bool downHeld() const { return states[1]; }
    bool starHeld() const { return states[3]; }
    unsigned long hashHoldDuration() const { return states[2] ? (millis() - hashPressStart) : 0; }
    unsigned long hashPressStartTime() const { return states[2] ? hashPressStart : 0; }
    // STAR helpers (symmetry with HASH)
    unsigned long starHoldDuration() const { return states[3] ? (millis() - starPressStart) : 0; }
    unsigned long starPressStartTime() const { return states[3] ? starPressStart : 0; }
    // Legacy right* methods retained but mapped to star for now (not used for menu)
    bool rightLongPressed() const { return false; }
    unsigned long rightHoldDuration() const { return 0; }
    // Use Defaults for long-press threshold
    static constexpr unsigned long LONG_PRESS_MS = Defaults::BUTTON_LONG_PRESS_MS;
    void dumpImmediateDebug() const; // optional
    // Counters
    uint32_t getPressCountUp() const { return pressEdges[0]; }
    uint32_t getPressCountDown() const { return pressEdges[1]; }
    uint32_t getPressCountHash() const { return pressEdges[2]; }
    uint32_t getPressCountStar() const { return pressEdges[3]; }
private:
    uint8_t pins[4];
    bool states[4];      // debounced current
    bool rawLevels[4];   // last raw read (active low)
    bool edgeFlags[4];   // true exactly in the cycle after a press edge
    uint32_t pressEdges[4];
    unsigned long stateSince[4];
    unsigned long hashPressStart = 0;
    unsigned long hashReleaseTime = 0;
    bool hashLongPressActive = false;
    unsigned long starPressStart = 0;
    unsigned long starReleaseTime = 0;
public:
    unsigned long hashLastReleaseTime() const { return hashReleaseTime; }
    unsigned long starLastReleaseTime() const { return starReleaseTime; }
};
