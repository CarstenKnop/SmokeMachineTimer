#include "ui/ButtonInput.h"

ButtonInput::ButtonInput(uint8_t upGpio, uint8_t downGpio, uint8_t hashGpio, uint8_t starGpio) {
    pins_[0] = upGpio; pins_[1] = downGpio; pins_[2] = hashGpio; pins_[3] = starGpio;
}

void ButtonInput::begin() {
    const uint32_t now = millis();
    for (int i = 0; i < 4; ++i) {
        pinMode(pins_[i], INPUT_PULLUP);
        raw_[i] = (digitalRead(pins_[i]) == LOW);
        rawSince_[i] = now;
        state_[i] = raw_[i];
        bootDown_[i] = raw_[i];
        swallow_[i] = raw_[i];           // a key that is already down must not trigger an action
        pressStart_[i] = now;
    }
}

void ButtonInput::update() {
    const uint32_t now = millis();
    hashLongEvent_ = false;
    hashClick_ = false;
    for (int i = 0; i < 4; ++i) { edge_[i] = false; releaseEdge_[i] = false; }
    for (int i = 0; i < 4; ++i) {
        const bool raw = (digitalRead(pins_[i]) == LOW);
        if (raw != raw_[i]) { raw_[i] = raw; rawSince_[i] = now; }
        if (raw_[i] != state_[i] && (now - rawSince_[i]) >= Defaults::BUTTON_DEBOUNCE_MS) {
            state_[i] = raw_[i];
            if (state_[i]) {
                pressStart_[i] = now;
                if (!swallow_[i]) edge_[i] = true;
                if (i == 2) hashLongSent_ = false;
            } else {
                if (!swallow_[i]) { releaseEdge_[i] = true; if (i == 2 && !hashLongSent_) hashClick_ = true; }
                swallow_[i] = false;
            }
        }
    }
    if (state_[2] && !swallow_[2] && !hashLongSent_ && (now - pressStart_[2]) >= LONG_PRESS_MS) {
        hashLongSent_ = true;
        hashLongEvent_ = true;
    }
}
