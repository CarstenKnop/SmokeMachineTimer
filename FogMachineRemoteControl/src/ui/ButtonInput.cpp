// ButtonInput.cpp
// Handles button polling and debouncing.
#include "ButtonInput.h"

ButtonInput::ButtonInput(uint8_t upGpio, uint8_t downGpio, uint8_t hashGpio, uint8_t starGpio) {
    pins[0] = upGpio; pins[1] = downGpio; pins[2] = hashGpio; pins[3] = starGpio;
    for (int i = 0; i < 4; ++i) {
        states[i] = false;
        rawLevels[i] = true;
        edgeFlags[i] = false;
        pressEdges[i] = 0;
        stateSince[i] = millis();
    }
}

void ButtonInput::begin() {
    for (int i = 0; i < 4; ++i) {
        pinMode(pins[i], INPUT_PULLUP);
        states[i] = false;
        rawLevels[i] = true;
        edgeFlags[i] = false;
    }
}

void ButtonInput::update() {
    static uint16_t stableCounters[4] = {0,0,0,0};
    // Clear edge flags at start of cycle
    for (int i=0;i<4;++i) edgeFlags[i] = false;
    unsigned long now = millis();
    for (int i = 0; i < 4; ++i) {
        int raw = digitalRead(pins[i]);
        bool activeLow = (raw == LOW);
        if (states[i] == activeLow) {
            if (stableCounters[i] < 1000) stableCounters[i]++; // stable
        } else {
            // change candidate
            if (stableCounters[i] > 2) {
                // Commit change
                states[i] = activeLow;
                stateSince[i] = now;
                stableCounters[i] = 0;
                if (states[i]) { // rising edge (pressed)
                    edgeFlags[i] = true;
                    pressEdges[i]++;
                    if (i == 2) { // hash button start hold timer
                        hashPressStart = now;
                    } else if (i == 3) { // star button start hold timer
                        starPressStart = now;
                    }
                } else {
                    if (i == 2) { // release resets long press state
                        hashPressStart = 0;
                        hashLongPressActive = false;
                        hashReleaseTime = now;
                    } else if (i == 3) {
                        starPressStart = 0;
                        starReleaseTime = now;
                    }
                }
            } else {
                stableCounters[i]++;
            }
        }
    }
    // Long press detection for hash (index 2)
    if (states[2]) {
        if (!hashLongPressActive && hashPressStart && (now - hashPressStart > LONG_PRESS_MS)) {
            hashLongPressActive = true;
        }
    }
}

// Legacy functions removed / replaced by inline implementations in header.

void ButtonInput::dumpImmediateDebug() const {
    unsigned long now = millis();
    int rU = digitalRead(pins[0]);
    int rD = digitalRead(pins[1]);
    int rH = digitalRead(pins[2]);
    int rS = digitalRead(pins[3]);
    Serial.printf("[BTN DBG-IMMEDIATE] RAW=%d%d%d%d DEB=%d%d%d%d EdgeCnt=%lu,%lu,%lu,%lu #Hold=%lums #Long=%d\n",
        rU,rD,rH,rS,
        states[0],states[1],states[2],states[3],
        (unsigned long)pressEdges[0],(unsigned long)pressEdges[1],(unsigned long)pressEdges[2],(unsigned long)pressEdges[3],
        hashHoldDuration(), hashLongPressActive ? 1:0);
}
