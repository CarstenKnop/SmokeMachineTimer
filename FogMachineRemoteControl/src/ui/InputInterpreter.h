// InputInterpreter.h
// Encapsulates main-screen input semantics: rising-edge detection, short vs long press,
// and menu-entry gating. Separates ButtonInput (debounce/edges) from behavior.
#pragma once
#include <Arduino.h>
#include "ButtonInput.h"
#include "menu/MenuSystem.h"

class InputInterpreter {
public:
    struct Events {
        bool shortHash = false; // short click on main screen
        bool longHash = false;  // long hold for menu entry
        bool starPress = false; // immediate star press
    };

    void resetOnMenuExit(unsigned long /*exitTime*/) {
        // Force fresh edge for next interactions
        armedHash_ = false;
        prevHeld_ = false;
    }

    Events update(const ButtonInput& btn, const MenuSystem& menu) {
        Events ev;
        unsigned long now = millis();
        bool inMenu = menu.isInMenu();
        bool heldNow = btn.hashHeld();
        // Press edge → arm
        if (btn.hashPressed()) {
            armedHash_ = true;
            downTime_ = now;
        }
        // Long press to enter menu (only outside menu)
        if (!inMenu && armedHash_ && (now - downTime_ >= ButtonInput::LONG_PRESS_MS)) {
            ev.longHash = true;
            armedHash_ = false;
        }
        // Release edge → short press if armed and not long
        if (!heldNow && prevHeld_) {
            if (!inMenu && armedHash_) {
                unsigned long dur = now - downTime_;
                if (dur < ButtonInput::LONG_PRESS_MS) ev.shortHash = true;
            }
            armedHash_ = false;
        }
        prevHeld_ = heldNow;
        // Star press is immediate when not in menu
        if (!inMenu && btn.starPressed()) ev.starPress = true;
        return ev;
    }
private:
    bool armedHash_ = false;
    bool prevHeld_ = false;
    unsigned long downTime_ = 0;
};
