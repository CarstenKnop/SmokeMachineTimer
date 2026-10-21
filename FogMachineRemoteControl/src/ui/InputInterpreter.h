// InputInterpreter.h - main-screen input semantics: '#' short click vs long press (menu entry).
#pragma once

#include <Arduino.h>
#include "ui/ButtonInput.h"

class InputInterpreter {
public:
    struct Events {
        bool shortHash = false;   // '#' released before the long-press threshold
        bool longHash = false;    // '#' held long enough: open the menu
    };

    // Re-arm after leaving the menu so a still-held '#' does not immediately act again.
    void resetOnMenuExit() { armed_ = false; }

    Events update(const ButtonInput& btn, bool inMenu) {
        Events ev;
        if (inMenu) { armed_ = false; return ev; }
        if (btn.hashPressed()) armed_ = true;
        if (armed_ && btn.hashLongEvent()) { ev.longHash = true; armed_ = false; }
        if (btn.hashClick() && armed_) { ev.shortHash = true; armed_ = false; }
        return ev;
    }

private:
    bool armed_ = false;
};
